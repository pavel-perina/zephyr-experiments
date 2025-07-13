Goal is to create program that logs data from accelerometer or any sensor.
It almost works, but ...
- version checking new file with fstat hangs
- if file is not created with write command and reused, `fs_open` hangs, but it works in shell
- if it survives all of this, it hangs on `fs_sync`

Other than that it would be nice to have option to erase fs or files.
