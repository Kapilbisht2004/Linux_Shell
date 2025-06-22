To compile first switch to linux or use wsl
then run command ----->   gcc -o linux_shell v3.c tinyexpr.c $(pkg-config --cflags --libs gtk+-3.0) -lcurl -lm
then a executional file will be created now 
to run it use command ----->  ./linux_shell
currently it also also shows hostname and also follows linux style detail of current directory working
implemented redirection,custom command,inbuild command
ENJOY
