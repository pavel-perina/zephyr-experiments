set terminal pngcairo size 894,894 enhanced font 'Monoid,8';
set output 'output.png';
set multiplot layout 2,1;
set datafile separator ',';
set xlabel 'Time (s)';
set grid;
set key top left;
set lmargin 8

set title 'Acceleration';
set ylabel 'Acceleration (m/s²)';
set yrange [-16:16]
plot '19.csv' using ($1/1000.0):2 title 'Accel X' with lines lc rgb "#a3be8c", \
     '19.csv' using ($1/1000.0):3 title 'Accel Y' with lines lc rgb "#5e81ac";

set title 'Angular Velocity';
set ylabel 'Angular Velocity (mrad/s)';
set yrange [-8:8]
plot '19.csv' using ($1/1000.0):4 title 'Gyro Z' with lines lc rgb "#d08770"

unset multiplot