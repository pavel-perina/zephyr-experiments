// filesystem
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/storage/flash_map.h>

// shell
#include <zephyr/shell/shell_uart.h>
#include <zephyr/console/console.h>
#include <zephyr/sys/base64.h>

// timers etc
#include <zephyr/kernel.h>

// for LED
#include <zephyr/drivers/gpio.h>

// GPIO definitions
static const struct gpio_dt_spec green_led = GPIO_DT_SPEC_GET(DT_ALIAS(led_green), gpios);
static const struct gpio_dt_spec red_led   = GPIO_DT_SPEC_GET(DT_ALIAS(led_red),   gpios);
static const struct gpio_dt_spec blue_led  = GPIO_DT_SPEC_GET(DT_ALIAS(led_blue),  gpios);

static bool fs_mounted = false;
static struct fs_mount_t mount;

////////////////////////////////////////////////////////////////////////////////////
//  ____  _          _ _                                                 _
// / ___|| |__   ___| | |   ___ ___  _ __ ___  _ __ ___   __ _ _ __   __| |___
// \___ \| '_ \ / _ \ | |  / __/ _ \| '_ ` _ \| '_ ` _ \ / _` | '_ \ / _` / __|
//  ___) | | | |  __/ | | | (_| (_) | | | | | | | | | | | (_| | | | | (_| \__ \
// |____/|_| |_|\___|_|_|  \___\___/|_| |_| |_|_| |_| |_|\__,_|_| |_|\__,_|___/
//

// Shell command implementations
static int cmd_ls(const struct shell *shell, size_t argc, char **argv) {
    struct fs_dir_t  dir;
    struct fs_dirent entry;

    fs_dir_t_init(&dir);
    int rc =fs_opendir(&dir, "/lfs");
    if (rc != 0) {
        shell_error(shell, "Cannot open directory, error: %d", rc);
        return -1;
    }

    while ((rc = fs_readdir(&dir, &entry)) == 0) {
        if (entry.name[0] == 0)
            break;
        shell_print(shell, "%s\t%zu bytes", entry.name, entry.size);
    }

    if (rc != 0) {
        shell_error(shell, "Error reading directory: %d", rc);
    }

    fs_closedir(&dir);
    return 0;
}


static int cmd_get(const struct shell *shell, size_t argc, char **argv) {
    if (argc < 2) {
        shell_error(shell, "Usage: get <filename>");
        return -1;
    }

    char filepath[64];
    snprintf(filepath, sizeof(filepath), "/lfs/%s", argv[1]);

    struct fs_file_t file;
    fs_file_t_init(&file);

    if (fs_open(&file, filepath, FS_O_READ) != 0) {
        shell_error(shell, "Cannot open file: %s", argv[1]);
        return -1;
    }
    shell_print(shell, "MIME-Version: 1.0");
    shell_print(shell, "Content-Type: application/octet-stream; name=\"%s\"", argv[1]);
    shell_print(shell, "Content-Transfer-Encoding: base64");
    shell_print(shell, "Content-Disposition: attachment; filename=\"%s\"", argv[1]);
    shell_fprintf(shell, SHELL_NORMAL, "\n"); // Flush

    uint8_t buffer[54]; // 54 bytes * 4/3 = 72 base64 chars
    ssize_t bytes_read;

    while ((bytes_read = fs_read(&file, buffer, sizeof(buffer))) > 0) {
        char b64_buffer[80]; // 72 + some margin
        size_t olen;

        if (base64_encode(b64_buffer, sizeof(b64_buffer), &olen, buffer, bytes_read) == 0) {
            b64_buffer[olen] = '\0';
            shell_print(shell, "%s", b64_buffer);
        } else {
            shell_error(shell, "Base64 encoding failed");
            break;
        }
    }

    if (bytes_read < 0) {
        shell_error(shell, "Read error: %d", (int)bytes_read);
    }

    fs_close(&file);
    return 0;
}

// Shell command: write data to file
static int cmd_write(const struct shell *shell, size_t argc, char **argv)
{
    if (argc < 2) {
        shell_print(shell, "Usage: write <size_kb> [pattern]");
        return -EINVAL;
    }

    int size_kb = atoi(argv[1]);
    uint8_t pattern = (argc > 2) ? atoi(argv[2]) : 0x55; // Default pattern

    if (size_kb <= 0 || size_kb > 64) {
        shell_error(shell, "Size must be 1-64 KB");
        return -EINVAL;
    }

    // Find next available filename
    char filename[32];
    int file_num = 1;
    struct fs_file_t file;

    while (file_num < 1000) {
        snprintf(filename, sizeof(filename), "/lfs/%d.dat", file_num);

        fs_file_t_init(&file);
        int ret = fs_open(&file, filename, FS_O_READ);
        if (ret == 0) {
            fs_close(&file);
            file_num++;
            continue;
        }
        break; // File doesn't exist, use this number
    }

    // Create and write file
    fs_file_t_init(&file);
    int ret = fs_open(&file, filename, FS_O_CREATE | FS_O_WRITE);
    if (ret < 0) {
        shell_error(shell, "Failed to create %s: %d", filename, ret);
        return ret;
    }

    // Write data in chunks
    uint8_t buffer[256];
    memset(buffer, pattern, sizeof(buffer));

    int total_bytes = size_kb * 1024;
    int written = 0;

    shell_print(shell, "Writing %d KB to %s...", size_kb, filename);

    while (written < total_bytes) {
        int chunk_size = MIN(sizeof(buffer), total_bytes - written);

        ssize_t bytes_written = fs_write(&file, buffer, chunk_size);
        if (bytes_written < 0) {
            shell_error(shell, "Write failed: %d", (int)bytes_written);
            fs_close(&file);
            return bytes_written;
        }

        written += bytes_written;

        // Show progress every 1KB
        if (written % 1024 == 0) {
            shell_print(shell, "Progress: %d/%d KB", written/1024, size_kb);
        }
    }

    fs_close(&file);
    shell_print(shell, "Successfully wrote %s (%d bytes)", filename, written);

    return 0;
}

// Shell command: mount filesystem
static int cmd_mount(const struct shell *shell, size_t argc, char **argv) {
    if (fs_mounted) {
        shell_print(shell, "Filesystem already mounted");
        return 0;
    }

    shell_print(shell, "Checking flash area...");
    const struct flash_area *fa;
    int rc = flash_area_open(FIXED_PARTITION_ID(qspi_storage), &fa);
    if (rc != 0) {
        shell_error(shell, "Flash area open failed: %d", rc);
        return rc;
    }
    flash_area_close(fa);
    shell_print(shell, "Flash area OK");

    // Mount filesystem
    FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(storage);
    mount.type = FS_LITTLEFS,
    mount.fs_data = &storage,
    mount.storage_dev = (void *)FIXED_PARTITION_ID(qspi_storage),
    mount.mnt_point = "/lfs",

    shell_print(shell, "Mounting filesystem...");
    rc = fs_mount(&mount);
    if (rc != 0) {
        shell_error(shell, "Failed to mount filesystem: %d", rc);
        gpio_pin_set_dt(&red_led, 1);
        return rc;
    }

    fs_mounted = true;
    gpio_pin_set_dt(&blue_led, 1);
    shell_print(shell, "Filesystem mounted successfully");
    return 0;
}


static int cmd_umount(const struct shell *shell, size_t argc, char **argv) {
    if (!fs_mounted) {
        shell_print(shell, "Filesystem not mounted");
        return 0;
    }

    int rc = fs_unmount(&mount);
    if (rc != 0) {
        shell_error(shell, "Failed to unmount filesystem: %d", rc);
        return rc;
    }

    fs_mounted = false;
    gpio_pin_set_dt(&blue_led, 0);
    shell_print(shell, "Filesystem unmounted successfully");
    return 0;
}

static int cmd_rm(const struct shell *shell, size_t argc, char **argv) {
    if (!fs_mounted) {
        shell_error(shell, "Filesystem not mounted");
        return -EINVAL;
    }

    if (argc < 2) {
        shell_error(shell, "Usage: rm <filename>");
        return -EINVAL;
    }

    char filepath[64];
    snprintf(filepath, sizeof(filepath), "/lfs/%s", argv[1]);

    int ret = fs_unlink(filepath);
    if (ret < 0) {
        shell_error(shell, "Failed to delete %s: %d", filepath, ret);
        return ret;
    }

    shell_print(shell, "Deleted %s", filepath);
    return 0;
}

static int cmd_format(const struct shell *shell, size_t argc, char **argv) {
    shell_warn(shell, "WARNING: This will erase all data on /lfs!");

    if (argc < 2 || strcmp(argv[1], "--yes") != 0) {
        shell_print(shell, "Use: format --yes to confirm");
        return -EINVAL;
    }

    int rc;

    // Unmount if mounted
    if (fs_mounted) {
        rc = fs_unmount(&mount);
        if (rc != 0) {
            shell_error(shell, "Failed to unmount filesystem: %d", rc);
            return rc;
        }
        fs_mounted = false;
        gpio_pin_set_dt(&blue_led, 0);
    }

    // Open and erase flash area
    const struct flash_area *fa;
    rc = flash_area_open(FIXED_PARTITION_ID(qspi_storage), &fa);
    if (rc != 0) {
        shell_error(shell, "flash_area_open failed: %d", rc);
        return rc;
    }

    rc = flash_area_erase(fa, 0, fa->fa_size);
    flash_area_close(fa);
    if (rc != 0) {
        shell_error(shell, "flash_area_erase failed: %d", rc);
        return rc;
    }

    shell_print(shell, "Filesystem formatted successfully. Use 'mount' to remount.");
    return 0;
}


// Register shell commands
SHELL_CMD_REGISTER(ls,     NULL, "List files", cmd_ls);
SHELL_CMD_REGISTER(get,    NULL, "Get file content", cmd_get);
SHELL_CMD_REGISTER(write,  NULL, "Write test file", cmd_write);
SHELL_CMD_REGISTER(mount,  NULL, "Mount filesystem", cmd_mount);
SHELL_CMD_REGISTER(umount, NULL, "Unmount filesystem", cmd_umount);
SHELL_CMD_REGISTER(rm,     NULL, "Remove file", cmd_rm);
SHELL_CMD_REGISTER(format, NULL, "Format filesystem (ERASES ALL FILES)", cmd_format);


int main() {
    bool devicesOk = true;

    // Initialize GPIO
    if (!gpio_is_ready_dt(&red_led) || !gpio_is_ready_dt(&green_led) || !gpio_is_ready_dt(&blue_led)) {
        printk("LED device not ready\n");
        devicesOk = false;
    }
    if (!devicesOk) {
        return -1;
    }

    gpio_pin_configure_dt(&red_led,   GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&green_led, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&blue_led,  GPIO_OUTPUT_INACTIVE);
    gpio_pin_set_dt(&green_led,  1);

    // NOTE: here we have shell thread running, program won't exit
    return 0;
}

