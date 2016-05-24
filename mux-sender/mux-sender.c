#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>
#include <ctype.h>
#include <sys/select.h>
#include <sys/time.h>

// For debug this tool
#ifdef DEBUG
#define szdebug(fmt, args...)    printf("[%s]#%d " fmt "\n", __FUNCTION__, __LINE__, ##args)
#else
#define szdebug(...)
#endif
//#define muxlog(fmt, args...)    do { if(strrchr(fmt, '\n') != NULL) printf("[%05d] " fmt, getpid(), ##args); else printf(fmt, ##args); }while(0)
#define muxlog(fmt, args...)    printf(fmt, ##args)

#define UNUSED(x)             ((void)(x))

#define help() do{ szdebug("HELP:%d", __LINE__); usage(argv[0]); }while(0)
#define CHECK_OUT(condition, statement, mark)    do { if(condition) {statement; goto mark;} }while(0)

// redirection file
#ifdef IDEN_PLATFORM
#define REDIRECT_FILE    "/home/logger/mux-sender-%05d.txt"
#else
#define REDIRECT_FILE    "/dev/null"
#endif

// MUX device prefix name
#define DEV_BASE_NAME    "/dev/ts0710mux"
// Max length for message
#define MAX_MSG_LEN      (1024)

// This enum corresponds the upper definition
enum {
    ID_INTERVAL,
    ID_TIMES,
    ID_CHANNEL,
    ID_STRING,
    ID_HEX_STRING,
    ID_ENDLINE,
    ID_RESPONSE,
    ID_ONESHOT,
    ID_HELP,
    ID_INVALID
};
struct mux_option {
    const char *name;    // option string
    char flag;           // whether argument needed
    const char *argname; // argument's name
    const char *des;     // description for this option
} opts[] = {
    [ID_INTERVAL]   = {"-interval",      1, "number",     "At least ms interval for resending the string. By default, set it as -1 to block waiting response"},
    [ID_TIMES]      = {"-times",         1, "number",     "Send message for num times. default as 1 and indicates oneshot message; <0 will send none; 0 send forever"},
    [ID_CHANNEL]    = {"-channel",       1, "number",     "MUX channel number in area[0, 15]. No default"},
    [ID_STRING]     = {"-string",        1, "string",     "Send the following string whose max EN characters shall be 1024"},
    [ID_HEX_STRING] = {"-hex-string",    1, "hex string", "Send the following string specified by HEX format which can only express 1024 EN characters at most"},
    [ID_ENDLINE]    = {"-endline",       1, "yes/no",     "Whether append <CR><LF> at the end of string. Default will append them"},
    [ID_ONESHOT]    = {"-oneshot",       1, "yes/no",     "Whether exit program when message is sended. Default set as no"},
    [ID_RESPONSE]   = {"-response",      1, "yes/no",     "Whether read the response. Default will read the response"},
    [ID_HELP]       = {"-help",          0, "null",       "Show this message"},
    [ID_INVALID]    = {NULL,             0, NULL,         "invalid end of options"}
};

int hex_userstr_to_buffer(char* buffer, size_t buffer_size/*_max*/,
                          const char* userstr, size_t userlen)
{
    int ret = 0;
    size_t i, j, inputlen = userlen;
    const unsigned char* input = (const unsigned char*)userstr;

    if(!buffer || !userstr || !userlen)
    {
        ret = EINVAL;
        goto out;
    }

    if(userlen > 2 && (input[0] == '0' && (input[1] == 'x' || input[1] == 'X')))
    {
        input += 2;
        inputlen -= 2;
    }

    memset(buffer, 0, buffer_size);

    for(i = 0, j = 0; i+1 < userlen && input[i] != 0; i+=2, ++j)
    {
        char high = 0, low = 0;

        if(j > buffer_size - 1)
        {
            ret = ENOBUFS;
            goto err;
        }

        high = toupper(input[i]) - '0';
        if(high > 9) high -= 7;
        low = toupper(input[i+1]) - '0';
        if(low > 9) low -= 7;

        buffer[j] = high * 16 + low;
    }
    buffer[j] = 0;
    goto out;

err:
    memset(buffer, 0, buffer_size);
out:
    return ret;
}

int read_option_value(void** value, const char* strval, const struct mux_option* option)
{
    char *endptr = NULL;

    if(!strval || !value || !option) return EINVAL;

    if(strcmp(option->argname, "number") == 0)
    {
        **(int**)value = (int)strtol(strval, &endptr, 10);
        if((errno == ERANGE) || (endptr == strval)) {
            szdebug("Error: %s. endptr=%p", strerror(errno), endptr);
            return EINVAL;
        }
    }
    else if(strcmp(option->argname, "string") == 0)
    {
        *(const char**)value = strval;
    }
    else if(strcmp(option->argname, "hex string") == 0)
    {
        size_t len = strlen(strval);
        if(len == 0)
        {
            return EINVAL;
        }
        if(hex_userstr_to_buffer((char*)value, MAX_MSG_LEN + 3, strval, len) != 0)
        {
            szdebug("HEX convert error");
        }
    }
    else if(strcmp(option->argname, "yes/no") == 0)
    {
        if(strcmp(strval, "yes") == 0)
        {
            **(int**)value = 1;
        }
        else if(strcmp(strval, "no") == 0)
        {
            **(int**)value = 0;
        }
        else
        {
            return EINVAL;
        }
    }
    else
    {
        return EINVAL;
    }

    return 0;
}

void append_usage()
{
#ifdef IDEN_PLATFORM
	printf("NOTE:\n");
	printf("    Since we cannot received SIGINT, SIGQUIT signal on IDEN. So we will always \n");
	printf("  keep background. All message of this program will saved in the following file:\n");
	printf("  %s", REDIRECT_FILE);
#endif
}

void usage(const char* progname)
{
    size_t max = 0;
    size_t arg = 0;
    size_t len = 0;
    size_t idx = 0;

    for(; opts[idx].name != NULL; ++idx)
    {
        len = strlen(opts[idx].name);
        max = max > len ? max : len;

        len = strlen(opts[idx].argname);
        arg = arg > len ? arg: len;
    }

    printf("Usage:\n");
    printf("    %s [options] [argument]\n", progname == NULL ? "UNKNOW" : progname);
    printf("Options\n");
    printf("    \033[7m%-*s\033[0m    \033[7m%-*s\033[0m    \033[7m%s\n\033[0m",
                (int)max, "option", (int)arg, "argument", "description");
    for(idx = 0; opts[idx].name != NULL; ++idx)
    {
        printf("    %-*s    %-*s    %s\n", (int)max, opts[idx].name,
            (int)arg, opts[idx].argname,
            opts[idx].des);
    }

    append_usage();
}

void work_background(int sig)
{
    UNUSED(sig);
    pid_t pid;
#ifdef IDEN_PLATFORM
	char fname[40] = {0};
    int  nullfd    = -1;
#else
    int nullfd = open(REDIRECT_FILE, O_RDWR);
#endif

#ifdef IDEN_PLATFORM
	sprintf(fname, REDIRECT_FILE, getpid());
	nullfd = open(fname, O_RDWR | O_CREAT | O_APPEND, S_IRWXU);
#endif

    pid=fork();
    if(pid < 0) {
        muxlog("Failed to create child process:%s\n", strerror(errno));
        return;
    }

    if(pid > 0) { // parent process
        muxlog("\nGoing to background...\n");
        muxlog("If you want to exit this program, please using 'kill -9 %d' to kill this program.\n", pid);

        // Since MUX channel handler should not close, it will error when U do this, so we could not exit the parent process.
		// FIXED : 这里可以退出paraent进程，因为文件描述符被复制，但是它们指向同一个file结构对象，所以即使父进程退出，系统也不会释放file结构对象，而是让计数器减1
        exit(0);
    }

    // new child process
    if(nullfd < 0) {
        muxlog("Cannot redirect streams\n");
    } else {
        dup2(nullfd, 0);
        dup2(nullfd, 1);
        dup2(nullfd, 2);
    }

    if(setsid() < 0) {
        muxlog("Create new session failed.\n");
    }

    //if(daemon(1,0) < 0)
    //{
    //    muxlog("%s\n", strerror(errno));
    //}
}

int reprint_char(char c, size_t count)
{
    char* string = (char*)calloc(count + 1, sizeof(char));

    if(!string) {
        muxlog("Error:No enough memory\n");
        return ENOMEM;
    }
    memset(string, isprint(c) ? c : '.', count);
    muxlog("%s\n", string);

    free(string);
    return 0;
}

void dump_data(const char* const buffer, const size_t len)
{
#define LINE_MAX_CHAR   (60)
#define CHAR_WIDTH      (3)
    size_t i, j, preend = 0;
    int width = LINE_MAX_CHAR;
    int curcol = 1;

    if(!buffer || !len) return;

    reprint_char('-', width);

    for(i = 0; i < len; ++i)
    {
        muxlog("%02X ", buffer[i]);
        curcol += CHAR_WIDTH;

        if(curcol >= LINE_MAX_CHAR)
        {
            muxlog("\n");
            for(j = preend; j <= i; ++j)
            {
                muxlog("%c  ", isprint(buffer[j]) ? buffer[j] : '.');
            }
            muxlog("\n");
            curcol = 1;
            preend = i + 1;
        }
    }

    if(curcol < LINE_MAX_CHAR)
    {
        muxlog("\n");
        for(j = preend; j < len; ++j)
        { // print the last not full line
            muxlog("%c  ", isprint(buffer[j]) ? buffer[j] : '.');
        }
    }

    muxlog("\n");
    reprint_char('-', width);
}

int send_message(int fd, const char* const buffer, const int send_times, const int ms_interval, const int response)
{
    int curtimes = 0; // How many times the message sent
    const char *const sendbuf = buffer;
    size_t sendlen = strlen(buffer);
    char recvbuf[MAX_MSG_LEN+1] = {0};
    int  enableInterval = ms_interval == -1 ? 0 : 1;

    fd_set readfs;
    struct timeval timeout;

    if(!sendbuf)
    {
        szdebug("No data input to send\n");
        return EINVAL;
    }

    FD_ZERO(&readfs);
    FD_SET(fd, &readfs);
    if(enableInterval) {
        memset(&timeout, 0, sizeof(struct timeval));
        timeout.tv_sec = ms_interval / 1000;
        timeout.tv_usec = ms_interval % 1000 * 1000;
    }

    curtimes = 0;
    while(curtimes < send_times || send_times == 0)
    {
        muxlog("sending data================>\n");
        dump_data(sendbuf, sendlen);
        if(write(fd, sendbuf, strlen(sendbuf)) < 0)
        {
            muxlog("Send error:%s\n", strerror(errno));
        }
#ifdef __HOST_PC_TEST__
        fsync(fd);
        sleep(1);
        szdebug("%ld", lseek(fd, 0, SEEK_SET));
#endif
        szdebug("Send ended");
        if(response)
        {
            int sel = -1;
            muxlog("Reading data<=================\n");
            if((send_times == 1) || (!enableInterval))
            { // oneshot message shall also block here if response needed
                sel = select(fd+1, &readfs, NULL, NULL, NULL);
            }
            else
            {
                sel = select(fd+1, &readfs, NULL, NULL, &timeout);
            }
            if(sel > 0)
            {
                size_t readlen = read(fd, recvbuf, MAX_MSG_LEN);
#ifdef __HOST_PC_TEST__
                szdebug("readlen:%ld", readlen);
#endif
                if(readlen > 0)
                {
                    dump_data(recvbuf, readlen);
                }
            }
            else if(sel == 0)
            {
                goto next;
            }
            else
            {
                muxlog("Error happend when receiving:%s\n", strerror(errno));
            }
        }

        if(enableInterval || response)
        {
            if(timeout.tv_sec != 0)
            {
                sleep(timeout.tv_sec);
            }
            if(timeout.tv_sec != 0)
            {
                usleep(timeout.tv_usec);
            }
        }
next:
        if(response && enableInterval)
        {
            memset(&timeout, 0, sizeof(struct timeval));
            timeout.tv_sec = ms_interval / 1000;
            timeout.tv_usec = ms_interval % 1000 * 1000;
        }
        ++curtimes;
    }

    return 0;
}

// Program entry function
int main(int argc, char* argv[])
{
    // function variables
    int ret = 0;

    // temp usage variables
    int  index = 0;  // option array index
    int  argid = 0;  // user argment index
    char *message  = NULL; // message that point to the string in argments

    // user argument variables
    int  channel   = -1; // MUX channel number
    int  interval  = -1;  // time between two sending action
    int  times     = 1;  // send the message times
    int  endline   = 1;  // default append "\r\n"
    int  oneshot   = 0;  // whether exit program after message sent
    int  response  = 1;  // whether read the response message

    // MUX operate variables
    char device[64] = {0};
    int  muxfd = -1;
    char buffer[MAX_MSG_LEN+3] = {0}; // Store message that will be sent

    // daemon this program
#ifndef IDEN_PLATFORM
    signal(SIGINT, work_background);
    signal(SIGQUIT, work_background);
#endif
    signal(SIGCHLD, SIG_IGN);

    // read user's specified
    argid = 1;
    while(argid < argc)
    {
        for(index = 0; opts[index].name != NULL; ++index)
        {
            if(strcmp(argv[argid], opts[index].name) == 0)
            {
                switch(index)
                {
                    case ID_INTERVAL:
                    {
                        int* value = &interval;
                        if(read_option_value((void**)&value, argv[++argid], &opts[index]) != 0)
                        {
                            szdebug("Interval arg invalid");
                            help();
                            ret = EINVAL;
                            goto out;
                        }
                        goto next;
                    }
                    case ID_TIMES:
                    {
                        int* value = &times;
                        if(read_option_value((void**)&value, argv[++argid], &opts[index]) != 0)
                        {
                            szdebug("Endless arg invalid");
                            help();
                            ret = EINVAL;
                            goto out;
                        }
                        goto next;
                    }
                    case ID_CHANNEL:
                    {
                        int* value = &channel;
                        if(read_option_value((void**)&value, argv[++argid], &opts[index]) != 0
                            || (channel < 0 || channel > 16))
                        {
                            szdebug("Channel arg invalid:%d", channel);
                            help();
                            ret = EINVAL;
                            goto out;
                        }
                        goto next;
                    }
                    case ID_STRING:
                    {
                        CHECK_OUT(message != NULL, ret = EINVAL, out);
                        message = argv[++argid];
                        strncpy(buffer, message, MAX_MSG_LEN);
                        goto next;
                    }
                    case ID_HEX_STRING:
                    {
                        CHECK_OUT(message != NULL, ret = EINVAL, out);
                        message = argv[++argid];
                        if(read_option_value((void**)(&buffer), message, &opts[index]) != 0)
                        {
                            szdebug("HEX string failed");
                            help();
                            ret = EINVAL;
                            goto out;
                        }
                        goto next;
                    }
                    case ID_ENDLINE:
                    {
                        int* value = &endline;
                        if(read_option_value((void**)&value, argv[++argid], &opts[index]) != 0)
                        {
                            szdebug("Endline arg invalid");
                            help();
                            ret = EINVAL;
                            goto out;
                        }
                        goto next;
                    }
                    case ID_RESPONSE:
                    {
                        int* value = &response;
                        if(read_option_value((void**)&value, argv[++argid], &opts[index]) != 0)
                        {
                            szdebug("Endline arg invalid");
                            help();
                            ret = EINVAL;
                            goto out;
                        }
                        goto next;
                    }
                    case ID_ONESHOT:
                    {
                        int* value = &oneshot;
                        if(read_option_value((void**)&value, argv[++argid], &opts[index]) != 0)
                        {
                            szdebug("Oneshot arg invalid");
                            help();
                            ret = EINVAL;
                            goto out;
                        }
                        goto next;
                    }
                    case ID_HELP:
                    default:
                    {
                        help();
                        ret = 0;
                        goto out;
                    }
                }
            }
            else if(index == (ID_INVALID -1))
            {
                szdebug("Invalid option:%s", argv[argid]);
                ret = EINVAL;
                help();
                goto out;
            }
        }
    next: // next option
        ++argid;
    }

    muxlog("channel=%d,interval=%d,response=%d,times=%d,endline=%d,message='%s'\n",
                channel, interval, response, times, endline, buffer);

    // FIXME : Maybe we should delete the character that out of the range 1024.
    //         If bug reported future, set the last ones if 'buffer' as '0'. I
    //         think that current action can fit the most requests.

    // check the user inputs
	if(channel == -1)
	{
		ret = EINVAL;
		help();
		goto out;
	}

    if(endline)
    {
        strcat(buffer, "\r\n");
    }

    if(times < 0)
    {
        muxlog("Warning: message will not be sent\n");
    }

#ifdef IDEN_PLATFORM
    work_background(SIGINT);
#endif

    // prepare MUX channel
    sprintf(device, "%s%d", DEV_BASE_NAME, channel);
#ifdef __HOST_PC_TEST__
    if((muxfd = open("null", O_RDWR | O_CREAT | O_TRUNC, S_IWUSR | S_IRUSR)) < 0)
#else
    szdebug("Opening... file - %s", device);
    if((muxfd = open(device, O_RDWR)) < 0)
#endif
    {
        ret = errno;
        muxlog("Cannot open device %s:%s\n", device, strerror(ret));
        goto out;
    }

    send_message(muxfd, buffer, times, interval, response);

    if(!oneshot)
    { // if not 'oneshot' program and reach here, hold the program live
        while(1) sleep(UINT_MAX);
    }

out:
    if(muxfd != -1)
    {
        close(muxfd);
    }

    return ret;
}


