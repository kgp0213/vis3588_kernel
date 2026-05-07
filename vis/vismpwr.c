#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <sys/ioctl.h>
#include <time.h>

#define MAX_RETRIES 5
#define RETRY_DELAY_MS 1
#define MAX_DATA_LEN 262

#define DEBUG 0  // 0关闭/1开启

//#pragma pack(push, 1)
struct command {
    int is_read;        //读写标志
    int dev_num;        // 0/1，-1 表示同时写 dsi0+dsi1
    int use_lpm;        // 1: LP模式(默认) 0: HS模式
    unsigned char read_len;  // 用于读 模式下的读取长度（1字节）
    unsigned char read_reg;  //用于读 模式下的读取长度  寄存器地址（1字节）
    unsigned char data[MAX_DATA_LEN]; // 数据内容 写模式下包含data_type/flags 在内容数据
};
//#pragma pack(pop)

void delay_ms(int ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;
    nanosleep(&ts, NULL);
}

int hex_to_int(char c) {
    c = tolower(c);
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

int parse_hex_byte(const char *str) {
    if (!str || strlen(str) > 2) return -1;

    // 处理单字符情况（如 "1" -> 0x01）
    if (strlen(str) == 1) {
        int val = hex_to_int(str[0]);
        return (val < 0) ? -1 : val; // 直接返回单个数字的值
    }
    // 处理双字符情况（如 "1a" -> 0x1A）
    int high = hex_to_int(str[0]);
    int low = hex_to_int(str[1]);
    if (high < 0 || low < 0) return -1;

    return (high << 4) | low;
}

int parse_args(int argc, char *argv[], struct command *cmd) {
    memset(cmd, 0, sizeof(struct command));
    cmd->dev_num = 0; //默认mipi_dsi
    cmd->use_lpm = 1; //默认LP模式
    //cmd->is_read=0;   //默认写入

    if (argc < 2) {
        fprintf(stderr, "Error: No arguments\n");
        return -1;
    }

    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        return 1;
    }

    int data_start = 1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0) {
            cmd->is_read = 1;
            data_start++;
        }
        else if (strcmp(argv[i], "-0") == 0) {
            cmd->dev_num = 0;
            data_start++;
        }
        else if (strcmp(argv[i], "-1") == 0) {
            cmd->dev_num = 1;
            data_start++;
        }
        else if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--both") == 0) {
            cmd->dev_num = -1;
            data_start++;
        }
        else if (strcmp(argv[i], "-hs") == 0 || strcmp(argv[i], "--hs") == 0) {
            cmd->use_lpm = 0;
            data_start++;
        }
        else if (strcmp(argv[i], "-lp") == 0 || strcmp(argv[i], "--lp") == 0) {
            cmd->use_lpm = 1;
            data_start++;
        }
    }

    if (!cmd->is_read && argc - data_start < 3) {
        fprintf(stderr, "Write cmd needs at least data_type delay payload_len\n");
        return -1;
    }

    if (cmd->is_read && cmd->dev_num < 0) {
        fprintf(stderr, "Read cmd does not support -b/--both\n");
        return -1;
    }

    if (cmd->is_read) {
        if (argc - data_start != 2) {
            fprintf(stderr, "Read cmd needs reg and len\n");
            return -1;
        }
        cmd->read_len = parse_hex_byte(argv[data_start]);
        cmd->read_reg = parse_hex_byte(argv[data_start+1]);
        if(DEBUG)
        {
            fprintf(stderr, "cmd->read_len is: %d; cmd->read_reg is:0x%02x\n",cmd->read_len,cmd->read_reg);
        }
        if (cmd->read_reg <= 0 || cmd->read_len <= 0) {
            fprintf(stderr, "Invalid hex value\n");
            return -1;
        }
    } else {
        int  payload_len;
        payload_len = parse_hex_byte(argv[data_start+2]);
        cmd->read_len =payload_len; //临时借用这个位置放置写入数据的有效长度
         if (argc - data_start < payload_len + 3) {
            fprintf(stderr, "Write cmd needs data_type delay payload_len and %d payload bytes\n", payload_len);
            return -1;
        }
        if (payload_len <= 0) {
            fprintf(stderr, "Data exceeds length err %d\n", payload_len);
            return -1;
        }

        //fprintf(stderr, "data_start:0x%02x\n",data_start);
        int data_type = parse_hex_byte(argv[data_start]);
        int delay = parse_hex_byte(argv[data_start + 1]);
        if (data_type < 0 || delay < 0) {
            fprintf(stderr, "Invalid data_type or delay\n");
            return -1;
        }
        cmd->data[0] = (unsigned char)data_type;
        cmd->data[1] = (unsigned char)delay;
        cmd->data[2] = (unsigned char)payload_len;
        cmd->data[3] = (unsigned char)(cmd->use_lpm ? 1 : 0);

        for (int i = 0; i < payload_len; i++) {
            int val = parse_hex_byte(argv[data_start + i + 3]);
            if (val < 0) {
                fprintf(stderr, "Invalid data byte %d\n", i);
                return -1;
            }
            if (DEBUG){
            fprintf(stderr, "val:0x%02x\n",val);}
            cmd->data[i + 4] = (unsigned char)val;
        }
    }
    return 0;
}

int execute_command(const char *dev_path, const void *cmd, int count) {
    int fd = open(dev_path, O_RDWR);
    if (fd < 0) {
        perror("Open device failed");
        return -1;
    }
    int retries = 0;
    while (retries < MAX_RETRIES) {
        if (write(fd, cmd, count) == count) {  // write() 成功执行后会返回写入的数量。return ret<0?ret:count;// 返回错误 或者 成功写入的字节数
            close(fd);
            return 0;
        }
        retries++;
        delay_ms(RETRY_DELAY_MS);
    }
    close(fd);
    return -1;
}

int write_register(struct command *cmd) {
    char dev_path[32];
    int total_len = (cmd->use_lpm ? 3 : 4) + cmd->read_len;
    unsigned char tx_buf[MAX_DATA_LEN + 4];
    int ret0 = -1, ret1 = -1;

    tx_buf[0] = cmd->data[0];
    tx_buf[1] = cmd->data[1];
    tx_buf[2] = cmd->data[2];
    if (cmd->use_lpm) {
        memcpy(&tx_buf[3], &cmd->data[4], cmd->read_len);
    } else {
        tx_buf[3] = cmd->data[3];
        memcpy(&tx_buf[4], &cmd->data[4], cmd->read_len);
    }

    if (DEBUG){
        fprintf(stderr, "Sending %d bytes (%s header)\n", total_len,
                cmd->use_lpm ? "legacy-3B" : "new-4B");
    }

    if (cmd->dev_num < 0) {
        snprintf(dev_path, sizeof(dev_path), "/dev/mipi_dsi0");
        ret0 = execute_command(dev_path, tx_buf, total_len);

        snprintf(dev_path, sizeof(dev_path), "/dev/mipi_dsi1");
        ret1 = execute_command(dev_path, tx_buf, total_len);

        if (ret0 < 0 || ret1 < 0) {
            fprintf(stderr, "write failed: dsi0=%s dsi1=%s\n",
                    ret0 == 0 ? "OK" : "FAIL",
                    ret1 == 0 ? "OK" : "FAIL");
        }

        return (ret0 == 0 && ret1 == 0) ? 0 : -1;
    }

    snprintf(dev_path, sizeof(dev_path), "/dev/mipi_dsi%d", cmd->dev_num);
    return execute_command(dev_path, tx_buf, total_len);
}

int read_register(struct command *cmd) {
    char dev_path[32];
    snprintf(dev_path, sizeof(dev_path), "/dev/mipi_dsi%d", cmd->dev_num);

    if (cmd->use_lpm) {
        unsigned char set_max_pkt[5] = {
            0x37, 0x00, 0x02,
            cmd->read_len, 0x00
        };

        if (execute_command(dev_path,set_max_pkt, sizeof(set_max_pkt)) < 0) {
            fprintf(stderr, "Set dt=0x37 packet failed,read len:0x%02x\n",cmd->read_len);
            return -1;
        }
        unsigned char read_cmd[4] = {
            0x06, 0x00, cmd->read_len,
            cmd->read_reg
        };
        if (execute_command(dev_path, read_cmd, sizeof(read_cmd)) < 0) {
            fprintf(stderr, "Read command failed,read reg:0x%02x\n",cmd->read_reg);
            return -1;
        }
    } else {
        unsigned char set_max_pkt_hs[6] = {
            0x37, 0x00, 0x02, 0x00,
            cmd->read_len, 0x00
        };
        if (execute_command(dev_path, set_max_pkt_hs, sizeof(set_max_pkt_hs)) < 0) {
            fprintf(stderr, "Set dt=0x37 packet failed (HS),read len:0x%02x\n",cmd->read_len);
            return -1;
        }
        unsigned char read_cmd_hs[5] = {
            0x06, 0x00, cmd->read_len, 0x00,
            cmd->read_reg
        };
        if (execute_command(dev_path, read_cmd_hs, sizeof(read_cmd_hs)) < 0) {
            fprintf(stderr, "Read command failed (HS),read reg:0x%02x\n",cmd->read_reg);
            return -1;
        }
    }

    int fd = open(dev_path, O_RDONLY);
    if (fd < 0) {
        perror("Open failed");
        return -1;
    }
    unsigned char *buf = malloc(cmd->read_len);
    if (!buf) {
        fprintf(stderr, "Malloc failed\n");
        close(fd);
        return -1;
    }
    ssize_t bytes_read = read(fd, buf, cmd->read_len);
    close(fd);

    if (bytes_read != cmd->read_len) {
        fprintf(stderr, "Read %zd/%hu bytes", bytes_read, cmd->read_len);
        if (bytes_read > 0) {
            fprintf(stderr, ", data:");
            for (ssize_t i = 0; i < bytes_read; i++) {
                fprintf(stderr, " %02x", buf[i]);
            }
        }
        fprintf(stderr, "\n");
        free(buf);
        return -1;
    }
    for (int i = 0; i < cmd->read_len; i++) {
        printf("%02x ", buf[i]);
    }
    printf("\n");
    free(buf);
    return 0;
}

void print_usage() {
    printf("Usage:\n");
    printf("  Write: vismpwr [-0|-1|-b] [-lp|-hs] data_type delay payload_len payload...\n");
    printf("    e.g. vismpwr -hs 29 00 03 51 12 34\n");
    printf("    e.g. vismpwr -b -hs 29 00 03 51 12 34\n");
    printf("    default write mode is LP; add -hs to send in HS mode\n");
    printf("  Read: vismpwr -r [-0|-1] [-lp|-hs] read_len read_reg\n");
    printf("    read does not support -b/--both\n");
    printf("    e.g. vismpwr -r 02 51          (LP read)\n");
    printf("    e.g. vismpwr -r -hs 02 51      (HS read for video mode)\n");
}

void print_version() {
    printf("vismpwr 2026-04-23\n");
}

int main(int argc, char *argv[]) {
    struct command cmd;
    int ret;

    if (argc >= 2 && (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--version") == 0)) {
        print_version();
        return 0;
    }
    ret = parse_args(argc, argv, &cmd);
    if (ret == 1) {
        print_usage();
        return 0;
    }
    if (ret < 0) {
        print_usage();
        return 1;
    }
    return cmd.is_read ? read_register(&cmd) : write_register(&cmd);
}
