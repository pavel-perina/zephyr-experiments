
Second iteration of data logger skeleton.

Note that if you are not careful enough,
you might overwrite internal flash so
make sure to have spare Raspberry Pi Pico
and way how to connect it to SWCLK/SWDIO
pins. Good luck & have fun!

Pavel Perina, 2025-07-13

## Example usage:

```
(.venv) [pavel@marten -=- ~/zephyrproject/my_projects/shell-02-qspi-fs]$ mpremote a1
Connected to MicroPython at /dev/ttyACM1
Use Ctrl-] or Ctrl-x to exit this shell
help
Please press the <Tab> button to see all available commands.
You can also use the <Tab> button to prompt or auto-complete all commands or its subcommands.
You can try to call commands with <-h> or <--help> parameter for more information.

Shell supports following meta-keys:
  Ctrl + (a key from: abcdefklnpuw)
  Alt  + (a key from: bf)
Please refer to shell documentation for more details.

Available commands:
  clear    : Clear screen.
  device   : Device commands
  devmem   : Read/write physical memory
             Usage:
             Read memory at address with optional width:
             devmem <address> [<width>]
             Write memory at address with mandatory width and value:
             devmem <address> <width> <value>
  get      : Get file content
  help     : Prints the help message.
  history  : Command history.
  kernel   : Kernel commands
  ls       : List files
  mount    : Mount filesystem
  rem      : Ignore lines beginning with 'rem '
  resize   : Console gets terminal screen size or assumes default in case the
             readout fails. It must be executed after each terminal width change
             to ensure correct text display.
  retval   : Print return value of most recent command
  shell    : Useful, not Unix-like shell commands.
  write    : Write test file
uart:~$ ls
Cannot open directory, error: -2
uart:~$ mount
Checking flash area...
Flash area OK
Mounting filesystem...
Filesystem mounted successfully
uart:~$ ls
1.dat   2048 bytes
2.dat   1024 bytes
3.dat   3072 bytes
4.dat   4096 bytes
5.dat   5120 bytes
uart:~$ write
Usage: write <size_kb> [pattern]  
uart:~$ write 1 42
Writing 1 KB to /lfs/6.dat...
Progress: 1/1 KB
Successfully wrote /lfs/6.dat (1024 bytes)
uart:~$ ls
1.dat   2048 bytes
2.dat   1024 bytes
3.dat   3072 bytes
4.dat   4096 bytes
5.dat   5120 bytes
6.dat   1024 bytes
uart:~$ get 6.dat
MIME-Version: 1.0
Content-Type: application/octet-stream; name="6.dat"
Content-Transfer-Encoding: base64
Content-Disposition: attachment; filename="6.dat"

KioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioq
KioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioq
KioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioq
KioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioq
KioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioq
KioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioq
KioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioq
KioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioq
KioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioq
KioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioq
KioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioq
KioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioq
KioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioq
KioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioq
KioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioq
KioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioq
KioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioq
KioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioq
KioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKioqKg==
uart:~$ %
```

## Extraction

On Windows, it's easy with Total commander. Just log serial console output
and then, trim irrelevant data in text editor such as `Notepad++` or `Visual Studio Code`.
Save it as `6.dat.b64` and from file menu in Total Commander, select to decode MIME file.

On Linux this can be accomplished by these commands - first writes end of file starting
from 6th line (can be done manually), second decodes it.
```sh
tail -n +6 6.dat.64 | base64 -d > 6.dat
```
