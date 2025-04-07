#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <ext2fs/ext2fs.h>
#include <unistd.h>
#include <linux/limits.h>
#include <optional>
#include <filesystem>
#include <string>
#include <iostream>
#include <fstream>
#include "cxxopts.hpp"

namespace fs = std::filesystem;

struct args {
    std::string block_device;
    uint64_t start_lba;
    uint64_t end_lba;
    std::string start_path;
    bool log_all_files = false;
    bool verbose = false;
};

struct traverse_ctx {
    ext2_filsys fs;
    uint64_t device_start;
    std::string path;
    uint64_t *count;
    struct args *args;
};

blk64_t get_first_block(ext2_filsys fs, ext2_ino_t ino)
{
    struct ext2_inode inode;
    errcode_t err;
    blk64_t pblock = 0;

    err = ext2fs_read_inode(fs, ino, &inode);
    if (err || inode.i_size == 0) {
        return 0;
    }

    err = ext2fs_bmap2(fs, ino, &inode, NULL, 0, 0, NULL, &pblock);
    if (err) {
        return 0;
    }

    return pblock;
}

static int dir_iterate_cb(
    ext2_dir_entry *dirent,
    int offset,
    int blocksize,
    char *buf,
    void *priv_data)
{
    if (dirent->inode == 0 || strcmp(dirent->name, ".") == 0 || strcmp(dirent->name, "..") == 0) {
        return 0;
    }

    struct traverse_ctx *ctx = (struct traverse_ctx *)priv_data;
    (*ctx->count)++;

    int16_t name_len = dirent->name_len & 0xFF;
    std::string name(dirent->name, name_len);
    std::string path =
     ctx->path.back() == '/' ? ctx->path + name : ctx->path + "/" + name;

    struct ext2_inode inode;
    errcode_t err = ext2fs_read_inode(ctx->fs, dirent->inode, &inode);
    if (err) {
        std::cerr << "Error reading inode " << dirent->inode << ": " << error_message(err) << std::endl;
        return 0; // skip on error
    }

    if (S_ISDIR(inode.i_mode)) {
        struct traverse_ctx new_ctx;
        new_ctx.path = path;
        new_ctx.fs = ctx->fs;
        new_ctx.count = ctx->count;
        new_ctx.device_start = ctx->device_start;
        new_ctx.args = ctx->args;

        int retval = ext2fs_dir_iterate(ctx->fs, dirent->inode, 0, NULL, dir_iterate_cb, &new_ctx);
        if (retval) {
            std::cerr << "Error iterating directory " << path << ": " << error_message(retval) << std::endl;
        }
    } else if (S_ISREG(inode.i_mode)) {
        blk64_t first_block = get_first_block(ctx->fs, dirent->inode);
        uint64_t first_lba = first_block * ctx->fs->blocksize / 512 + ctx->device_start;
        if (ctx->args->log_all_files) {
            std::cerr << "File: " << path << " (inode: " << dirent->inode << ", first block: " << first_block << ", first lba: " << first_lba << ")" << std::endl;
        }
        if (first_lba >= ctx->args->start_lba && first_lba <= ctx->args->end_lba) {
            std::cout << path << " (inode: " << dirent->inode << ", first lba: " << first_lba << ")" << std::endl;
        }
    }
    return 0;
}

std::optional<std::string> absolute_path(const char *path) {
    if (path[0] == '/') {
        return std::string(path);
    } else {
        char *cwd = getcwd(NULL, 0);
        if (cwd) {
            std::string result = std::string(cwd) + "/" + path;
            free(cwd);
            return result;
        }
    }
    return std::nullopt;
}

std::string resolve_link(const char *path) {
    char resolved_path[PATH_MAX];
    if (realpath(path, resolved_path) == NULL) {
        std::cerr << "Error resolving path: " << path << std::endl;
        return std::string(path);
    }
    return std::string(resolved_path);
}

std::optional<std::string> sys_block_path(const char *dev, bool verbose = false) {
    fs::path sys_block("/sys/block");
    std::string dev_name(dev);
    const int max_depth = 4;
    if (dev_name.find("/dev/") == 0) {
        dev_name = dev_name.substr(5);
    }
    try {
        fs::recursive_directory_iterator it(sys_block, fs::directory_options::follow_directory_symlink), end;
        for (; it != end; ++it) {
            const fs::directory_entry &entry = *it;
            if (verbose) {
                std::cerr << "sys_block_path: checking " << entry.path() << std::endl;
            }
            if (entry.is_directory() && entry.path().filename() == dev_name) {
                return entry.path().string();
            }
            if (it.depth() > max_depth) {
                it.disable_recursion_pending();
            }
        }
    } catch (const fs::filesystem_error &e) {
        std::cerr << "Error walking /sys/block: " << e.what() << std::endl;
    }
    return std::nullopt;
}

uint64_t device_start_lba(const std::string &sys_block_path) {
    std::string path = sys_block_path + "/start";
    std::ifstream start_file(path);
    if (!start_file.is_open()) {
        return 0;
    }
    uint64_t start_lba;
    start_file >> start_lba;
    return start_lba;
}

std::optional<args> parse_args(int argc, const char **argv) {
    cxxopts::Options options(argv[0], "List files in a block device");
    options.add_options()
        ("b,block", "Block device", cxxopts::value<std::string>())
        ("p,path", "Path to start from", cxxopts::value<std::string>())
        ("s,start", "Start LBA", cxxopts::value<uint64_t>())
        ("e,end", "End LBA", cxxopts::value<uint64_t>())
        ("l,log", "Log all files", cxxopts::value<bool>()->default_value("false"))
        ("v,verbose", "Verbose output", cxxopts::value<bool>()->default_value("false"))
        ("h,help", "Show help");

    try {
        auto result = options.parse(argc, argv);
        if (result.count("help")) {
            std::cout << options.help() << std::endl;
            return std::nullopt;
        }

        args parsed_args;
        parsed_args.block_device = result["block"].as<std::string>();
        parsed_args.start_lba = result["start"].as<uint64_t>();
        parsed_args.end_lba = result["end"].as<uint64_t>();
        parsed_args.start_path = result["path"].as<std::string>();
        parsed_args.log_all_files = result["log"].as<bool>();
        parsed_args.verbose = result["verbose"].as<bool>();

        return parsed_args;
    } catch (const cxxopts::exceptions::exception &e) {
        std::cerr << e.what() << std::endl;
        return std::nullopt;
    }
}

int main(int argc, const char **argv) {
    std::optional<args> parsed_args = parse_args(argc, argv);
    if (!parsed_args) {
        return 1;
    }

    std::string device = resolve_link(parsed_args->block_device.c_str());
    if (parsed_args->verbose) {
        std::cerr << "Resolved device: " << device << std::endl;
    }

    std::optional<std::string> block_path = sys_block_path(device.c_str(), parsed_args->verbose);
    if (!block_path) {
        std::cerr << "Error finding block device path in /sys/block" << std::endl;
        return 1;
    }

    uint64_t device_start = device_start_lba(*block_path);
    if (parsed_args->verbose) {
        std::cerr << "Device start LBA: " << device_start << std::endl;
    }

    auto start_path = absolute_path(parsed_args->start_path.c_str());
    if (!start_path) {
        std::cerr << "Error converting path to absolute: " << parsed_args->start_path << std::endl;
        return 1;
    }

    ext2_filsys fs;
    errcode_t retval;
    int flags = 0; // Read-only mode
    retval = ext2fs_open(device.c_str(), flags, 0, 0, unix_io_manager, &fs);
    if (retval) {
        std::cerr << "Error opening filesystem: " << error_message(retval) << std::endl;
        return 1;
    }

    if (fs->super->s_magic != EXT2_SUPER_MAGIC) {
        std::cerr << "Not a valid ext2/ext3/ext4 filesystem" << std::endl;
        ext2fs_close(fs);
        return 1;
    }

    if (parsed_args->verbose) {
        std::cerr << "Opening filesystem on " << device << " (block size " << fs->blocksize << " bytes)" << std::endl;
    }

    ext2_ino_t start_ino = EXT2_ROOT_INO;
    struct ext2_inode inode;
    retval = ext2fs_namei(fs, EXT2_ROOT_INO, EXT2_ROOT_INO, start_path->c_str(), &start_ino);
    if (retval) {
        std::cerr << "Error looking up path " << start_path->c_str() << ": " << error_message(retval) << std::endl;
        ext2fs_close(fs);
        return 1;
    }

    if (parsed_args->verbose) {
        std::cerr << "Starting from path " << start_path->c_str() << " (inode " << start_ino << ")" << std::endl;
    }

    {
        int flags = 0;
        uint64_t count = 0;
        struct traverse_ctx ctx;
        ctx.fs = fs;
        ctx.path = *start_path;
        ctx.count = &count;
        ctx.args = &parsed_args.value();
        ctx.device_start = device_start;
        void *priv_data = &ctx;
        char *block_buf = NULL;
        retval = ext2fs_dir_iterate(fs, start_ino, flags, block_buf, dir_iterate_cb, priv_data);
        if (retval) {
            std::cerr << "Error iterating directory: " << error_message(retval) << std::endl;
            ext2fs_close(fs);
            return 1;
        }
        if (parsed_args->verbose) {
            std::cerr << "Total files scanned: " << count << std::endl;
        }
    }

    ext2fs_close(fs);
    return 0;
}
