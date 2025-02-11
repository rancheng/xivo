
#
#python scripts/pyxivo_pcw.py \
#    -npts 1000 \
#    -motion_type lissajous \
#    -xlim -20 20 \
#    -ylim -20 20 \
#    -zlim -20 20 \
#    -vision_dt 0.01 \
#    -noise_accel 1e-9 \
#    -noise_gyro 1e-9 \
#    -cfg cfg/pcw.json \
#    -use_viewer
#

python scripts/pyxivo_pcw.py \
    -motion_type checkerboard_traj \
    -noise_accel 1e-9 \
    -noise_gyro 1e-9 \
    -cfg cfg/pcw.json \
    -tracker_only \
    -viewer_cfg cfg/pcw_viewer_tracker_only.json \
    -use_viewer
