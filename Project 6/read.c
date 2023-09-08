int
main(int argc, char *argv[])
{
    const char *path;
    int fd1, fd2, fd3;

    if (argc != 2)
        mu_die("%s PATH", argv[0]);

    path = argv[1];

    fd1 = open(path, O_RDWR|O_CREAT|O_TRUNC, S_IRUSR|S_IWUSR);
    fd2 = dup(fd1);
    fd3 = open(path, O_RDWR);
    write(fd1, "Hello,", 6); 
    write(fd2, " World!", 7); 
    lseek(fd2, 0, SEEK_SET);
    write(fd1, "Guten,", 6); 
    write(fd3, "Salut,", 6); 

    close(fd1);
    close(fd2);
    close(fd3);

    return 0;
}