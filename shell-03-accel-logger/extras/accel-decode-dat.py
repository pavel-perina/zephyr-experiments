import struct
import sys
import os

def convert_bin_to_csv(input_file, output_file):
    """Convert a binary file with log_entry_t structures to CSV."""
    entry_size = 16  # sizeof(log_entry_t): int64_t (8) + 3*int16_t (6) + padding (2)
    with open(input_file, 'rb') as f_in, open(output_file, 'w') as f_out:
        f_out.write("uptime_ms,accel_x,accel_y,gyro_z\n")
        binary_data = f_in.read()

        if len(binary_data) % entry_size != 0:
            print(f"Warning: File size {len(binary_data)} is not a multiple of {entry_size}")

        for i in range(0, len(binary_data), entry_size):
            if i + entry_size > len(binary_data):
                break
            uptime_ms, accel_x, accel_y, gyro_z, padding = struct.unpack('<qhhhh', binary_data[i:i+entry_size])
            f_out.write(f"{uptime_ms},{accel_x/1000.0},{accel_y/1000.0},{gyro_z/1000.0}\n")

        print(f"Converted {len(binary_data)//entry_size} entries to {output_file}")

if __name__ == '__main__':
    if len(sys.argv) < 2:
        print("Usage: python bin_to_csv.py <input_binary_file> [output_csv_file]")
        sys.exit(1)

    input_file = sys.argv[1]
    if not os.path.exists(input_file):
        print(f"Input file {input_file} does not exist")
        sys.exit(1)

    output_file = sys.argv[2] if len(sys.argv) > 2 else os.path.splitext(input_file)[0] + '.csv'
    convert_bin_to_csv(input_file, output_file)
