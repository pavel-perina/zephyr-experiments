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

// for sensor
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

// Logger flag
static atomic_t g_logger_enabled = ATOMIC_INIT(1);
#define TIMEOUT 20
static int g_file_num = 1;


// GPIO definitions
static const struct gpio_dt_spec green_led = GPIO_DT_SPEC_GET(DT_ALIAS(led_green), gpios);
static const struct gpio_dt_spec red_led   = GPIO_DT_SPEC_GET(DT_ALIAS(led_red),   gpios);
static const struct gpio_dt_spec blue_led  = GPIO_DT_SPEC_GET(DT_ALIAS(led_blue),  gpios);

const struct device *const sensor = DEVICE_DT_GET(DT_ALIAS(imu_sensor));


/********************************************************************************
 *  ____  _          _ _                                                 _
 * / ___|| |__   ___| | |   ___ ___  _ __ ___  _ __ ___   __ _ _ __   __| |___
 * \___ \| '_ \ / _ \ | |  / __/ _ \| '_ ` _ \| '_ ` _ \ / _` | '_ \ / _` / __|
 *  ___) | | | |  __/ | | | (_| (_) | | | | | | | | | | | (_| | | | | (_| \__ \
 * |____/|_| |_|\___|_|_|  \___\___/|_| |_| |_|_| |_| |_|\__,_|_| |_|\__,_|___/
 *
 */

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

static int cmd_logger(const struct shell *shell, size_t argc, char **argv)
{
    if (argc != 2) {
        shell_print(shell, "Usage: logger <on|off>");
        return -EINVAL;
    }

    if (strcmp(argv[1], "on") == 0) {
        atomic_set(&g_logger_enabled, 1);
        shell_print(shell, "Logger ON");
    } else if (strcmp(argv[1], "off") == 0) {
        atomic_set(&g_logger_enabled, 0);
        shell_print(shell, "Logger OFF");
    } else {
        shell_error(shell, "Invalid argument: %s", argv[1]);
        return -EINVAL;
    }

    return 0;
}

SHELL_CMD_REGISTER(ls,     NULL, "List files", cmd_ls);
SHELL_CMD_REGISTER(get,    NULL, "Get file content", cmd_get);

SHELL_CMD_REGISTER(logger, NULL, "Toggle logger: logger <on|off>", cmd_logger);

SHELL_CMD_REGISTER(write,  NULL, "Write test file", cmd_write); // temporary

struct log_entry_t {
    int64_t uptime_ms;
    int16_t accel_x;
    int16_t accel_y;
    int16_t gyro_z;
    int16_t padding;
};

static struct log_entry_t log_entry;

void logger_thread(void) {
    struct fs_file_t file;
    char filename[32];
    snprintf(filename, sizeof(filename), "/lfs/%d.dat", g_file_num);
    printk("Filename %s will be used.\n", filename);
    printk("Sleeping\n");
    k_sleep(K_MSEC(1000*TIMEOUT));
    printk("Entering logging phase\n");

    memset(&file, 0, sizeof(file));
    fs_file_t_init(&file);
    k_sleep(K_MSEC(1000));
    gpio_pin_set_dt(&blue_led, 1);
    // The following command hangs
    int ret = fs_open(&file, filename, FS_O_CREATE | FS_O_WRITE);
    //int ret = 0;
    if (ret < 0) {
        printk("Failed to create %s: %d\n", filename, ret);
        gpio_pin_set_dt(&red_led, 1);
        return;
    }
    printk("Created %s\n", filename);
    gpio_pin_set_dt(&green_led, 0);

    int counter = 0;
    struct sensor_value accel_xyz[3];
    struct sensor_value gyro_z;
    while(1) {
        k_sleep(K_MSEC(100));
        ret = sensor_sample_fetch(sensor);
        if (ret >= 0) {
            ret = sensor_channel_get(sensor, SENSOR_CHAN_ACCEL_XYZ, accel_xyz);
        }
        if (ret >= 0) {
            ret = sensor_channel_get(sensor, SENSOR_CHAN_GYRO_Z, &gyro_z);
        }
        if (ret >= 0 ) {
            // 16 bytes, 20 times per second =

            log_entry.uptime_ms = k_uptime_get();
            log_entry.accel_x = accel_xyz[0].val1 * 1000 + accel_xyz[0].val2 / 1000;
            log_entry.accel_y = accel_xyz[1].val1 * 1000 + accel_xyz[1].val2 / 1000;
            log_entry.gyro_z  = gyro_z.val1 * 1000 + gyro_z.val2 / 1000; // mrad/s
            log_entry.padding = 0;
            printk("time=%lld ,ax=%d ,ay=%d, gz=%d mrad/s\n", log_entry.uptime_ms, log_entry.accel_x, log_entry.accel_y, log_entry.gyro_z );

            ret = fs_write(&file, (const void *)&log_entry, sizeof(struct log_entry_t));
            if (ret < 0) {
                 gpio_pin_set_dt(&red_led, 1);
            }
            if (atomic_get(&g_logger_enabled)) {
                gpio_pin_toggle_dt(&blue_led);
            }
        }

        counter++;
#if 1
        if (counter % 20 == 0) {
            int sync_ret = fs_sync(&file);
            if (sync_ret < 0) {
                printk("fs_sync failed: %d\n", sync_ret);
            } else {
                printk("Sync\n");
            }
        } else {
        }
#endif
    }
}

//K_THREAD_DEFINE(logger_thread_id, 1024, logger_thread, NULL, NULL, NULL, 5, 0, 0);


static int mount_fs() {
   // Mount filesystem
    printk("Checking flash area...\n");
    const struct flash_area *fa;
    int rc;
    rc = flash_area_open(FIXED_PARTITION_ID(qspi_storage), &fa);
    if (rc != 0) {
        printk("Flash area open failed: %d\n", rc);
        return rc;
    }
    flash_area_close(fa);
    printk("Flash area OK\n");

    // Mount filesystem
    FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(storage);
    static struct fs_mount_t lfs_mount = {
        .type = FS_LITTLEFS,
        .fs_data = &storage,
        .storage_dev = (void *)FIXED_PARTITION_ID(qspi_storage),
        .mnt_point = "/lfs",
    };

    printk("Mounting filesystem...\n");
    rc = fs_mount(&lfs_mount);
    if (rc != 0) {
        printk("Failed to mount filesystem: %d\n", rc);
        gpio_pin_set_dt(&red_led, 1);
        return rc;
    }

    printk("Filesystem mounted successfully\n");
    return 0;
}

#if 0

// This hangs
static int find_free_filenum() {
    gpio_pin_set_dt(&blue_led, 1);
    char filename[32];
    struct fs_dirent stat;
    while (g_file_num < 100) {
        snprintf(filename, sizeof(filename), "/lfs/%d.dat", g_file_num);
        int ret = fs_stat(filename, &stat);
        if (ret < 0) {
            // File doesn't exist, we found our free number
            gpio_pin_set_dt(&blue_led, 0);
            return 0;
        }
        g_file_num++;
    }
    gpio_pin_set_dt(&blue_led, 0);
    printk("No unused file found.");
    return -1;
}

#else

static int find_free_filenum() {
    char filename[32];
    struct fs_file_t file;
    while (g_file_num < 100) {
        snprintf(filename, sizeof(filename), "/lfs/%d.dat", g_file_num);
        fs_file_t_init(&file);
        int ret = fs_open(&file, filename, FS_O_READ);
        if (ret < 0)
            return 0; // file does not exist
        fs_close(&file);
        g_file_num++;
    }
    return -1;
}

#endif


int main(void)
{
    // Initialize GPIO
    if (!gpio_is_ready_dt(&red_led) || !gpio_is_ready_dt(&green_led) || !gpio_is_ready_dt(&blue_led)) {
        printk("LED device not ready\n");
        return -1;
    }
    gpio_pin_configure_dt(&green_led, GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&red_led,   GPIO_OUTPUT_INACTIVE);
    gpio_pin_configure_dt(&blue_led,  GPIO_OUTPUT_INACTIVE);

     if (!device_is_ready(sensor)) {
        printk("Sensor %s not ready!\n", sensor->name);
        gpio_pin_set_dt(&red_led,  1);
        return -1;
    } else {
        printk("Sensor OK\n");
    }

    int ret = mount_fs();
    if(ret) {
        gpio_pin_set_dt(&red_led,  1);
        return -1;
    }
    ret = find_free_filenum();
    if (ret < 0) {        
        gpio_pin_set_dt(&red_led,  1);
        return -1;
    }
    
    gpio_pin_set_dt(&green_led,  1);

    printk("Type logger off in %d seconds to disable logger\n", TIMEOUT);

    // NOTE: here we have shell thread and logger thread running,
    //       program won't exit

    logger_thread(); /// why not, we have shell thread
    return 0;
}