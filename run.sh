sudo socat PTY,raw,link=/dev/ttyGRBL,echo=0,group-late=dialout,mode=660 "EXEC:'sudo ./build/grblHAL_sim -n -s step.out -b block.out',pty,raw,echo=0"

