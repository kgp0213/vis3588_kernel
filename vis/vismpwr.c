#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <sys/ioctl.h>
#include <time.h>
#include <stdint.h>

/*
 * 只对"可重试"的瞬时错误重试，且最多 2 次。
 * 内核侧已经做了 CRI 串行化 + 100ms x3 的等待，这里再盲目重试
 * 确定性错误（EINVAL 等）没有意义，还可能把已经发出去的命令重发一遍。
 */
#define MAX_RETRIES 2
#define RETRY_DELAY_MS 1
#define MAX_DATA_LEN 262

#define DEBUG 0  // 0关闭/1开启

//#pragma pack(push, 1)
struct command {
    int is_read;        //读写标志
    int dev_num;        // 0/1，-1 表示同时写 dsi0+dsi1
    int use_lpm;        // 1: LP模式(默认) 0: HS模式
    int batch;          // 1: 批量文件模式
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
        else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            cmd->batch = 1;
            data_start += 2;
            i++;
        }
        else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
            cmd->batch = 2;
            data_start += 2;
            i++;
        }
    }

    if (cmd->batch)
        return 0;

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
        /*
         * read_reg <= 0 同时拦截两种情况:
         *   a) parse_hex_byte() 解析失败返回 -1
         *   b) 寄存器地址 0x00
         * MIPI DCS 中 0x00 为 NOP/保留, 面板寄存器 0x00 无回读意义。
         * 若将来需要读 0x00, 将 <= 0 改为 < 0 即可。
         */
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

static int is_transient_error(int err)
{
    return err == ETIMEDOUT || err == EBUSY || err == EAGAIN || err == EINTR;
}

static int dev_write_fd(int fd, const char *dev_path,
                        const void *cmd, int count) {
    int attempt = 0;
    int delay = RETRY_DELAY_MS;

    for (;;) {
        ssize_t n = write(fd, cmd, count);
        if (n == count)
            return 0;

        int err = (n < 0) ? errno : 0;

        /* 确定性错误、短写、或重试次数用尽：直接报错退出 */
        if (n >= 0 || !is_transient_error(err) || attempt >= MAX_RETRIES) {
            if (n < 0)
                fprintf(stderr,
                        "write %s failed: %s (%d), %d retries\n",
                        dev_path, strerror(err), err,
                        attempt);
            else
                fprintf(stderr,
                        "write %s short write: %zd/%d, %d retries\n",
                        dev_path, n, count, attempt);
            break;
        }

        /* 只对瞬时错误退避重试：1ms, 2ms */
        attempt++;
        delay_ms(delay);
        delay *= 2;
    }

    return -1;
}

int execute_command(const char *dev_path, const void *cmd, int count) {
    int fd = open(dev_path, O_RDWR);
    int ret;

    if (fd < 0) {
        fprintf(stderr, "open %s failed: %s (%d)\n",
                dev_path, strerror(errno), errno);
        return -1;
    }

    ret = dev_write_fd(fd, dev_path, cmd, count);
    close(fd);
    return ret;
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

/*
 * 批量模式：每行一条命令，格式与命令行一致
 *     data_type delay payload_len payload...
 * 空行和 # 开头的行忽略。全局 -hs/-lp/-0/-1 对整个文件生效。
 * 一次 open，避免每条命令都 fork/exec + open/close。
 */
static int batch_from_file(const char *path, struct command *proto) {
    FILE *f = fopen(path, "r");
    char line[1024];
    char dev_path[32];
    unsigned char tx_buf[MAX_DATA_LEN + 4];
    int fd, lineno = 0, ok = 0, fail = 0;

    if (!f) {
        fprintf(stderr, "open %s failed: %s (%d)\n",
                path, strerror(errno), errno);
        return -1;
    }

    snprintf(dev_path, sizeof(dev_path), "/dev/mipi_dsi%d", proto->dev_num);
    fd = open(dev_path, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "open %s failed: %s (%d)\n",
                dev_path, strerror(errno), errno);
        fclose(f);
        return -1;
    }

    while (fgets(line, sizeof(line), f)) {
        char *p = line, *tok;
        int dt, delay, plen, i, total;

        lineno++;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0' || *p == '\n' || *p == '#' || *p == '\r')
            continue;

        tok = strtok(p, " \t\r\n");
        if (!tok) continue;
        dt = parse_hex_byte(tok);
        tok = strtok(NULL, " \t\r\n"); if (!tok) continue;
        delay = parse_hex_byte(tok);
        tok = strtok(NULL, " \t\r\n"); if (!tok) continue;
        plen = parse_hex_byte(tok);

        if (dt < 0 || delay < 0 || plen <= 0) {
            fprintf(stderr, "line %d: bad header\n", lineno);
            fail++;
            continue;
        }

        tx_buf[0] = (unsigned char)dt;
        tx_buf[1] = (unsigned char)delay;
        tx_buf[2] = (unsigned char)plen;

        if (proto->use_lpm) {
            total = 3 + plen;
            for (i = 0; i < plen; i++) {
                tok = strtok(NULL, " \t\r\n");
                if (!tok || parse_hex_byte(tok) < 0) {
                    fprintf(stderr, "line %d: bad payload\n", lineno);
                    break;
                }
                tx_buf[3 + i] = parse_hex_byte(tok);
            }
            if (i != plen) { fail++; continue; }
        } else {
            total = 4 + plen;
            tx_buf[3] = 0;      /* HS: flags=0 */
            for (i = 0; i < plen; i++) {
                tok = strtok(NULL, " \t\r\n");
                if (!tok || parse_hex_byte(tok) < 0) {
                    fprintf(stderr, "line %d: bad payload\n", lineno);
                    break;
                }
                tx_buf[4 + i] = parse_hex_byte(tok);
            }
            if (i != plen) { fail++; continue; }
        }

        if (dev_write_fd(fd, dev_path, tx_buf, total) < 0) {
            fprintf(stderr, "line %d: write failed\n", lineno);
            fail++;
        } else {
            ok++;
        }
    }

    close(fd);
    fclose(f);
    printf("batch done: %d ok, %d failed\n", ok, fail);
    return fail ? -1 : 0;
}

/* ---- 批量 ioctl（DSI2_IOC_BATCH）---- */
struct dsi2_batch_req {
    unsigned int count;
    unsigned int len;
    unsigned long long data;
};

#define DSI2_IOC_MAGIC   'D'
#define DSI2_IOC_BATCH   _IOW(DSI2_IOC_MAGIC, 1, struct dsi2_batch_req)

static int batch_ioctl_from_file(const char *path, struct command *proto)
{
    FILE *f = fopen(path, "r");
    char line[1024];
    int lineno = 0, count = 0;
    size_t cap = 1024 * 1024, len = 0;
    unsigned char *buf;
    char dev_path[32];
    struct dsi2_batch_req req;
    int fd, ret;

    if (!f) {
        fprintf(stderr, "open %s failed: %s (%d)\n", path, strerror(errno), errno);
        return -1;
    }

    buf = malloc(cap);
    if (!buf) { fclose(f); return -1; }

    while (fgets(line, sizeof(line), f)) {
        char *p = line, *tok;
        int dt, delay, plen, i;

        lineno++;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '\n' || *p == '#' || *p == '\r') continue;

        tok = strtok(p, " \t\r\n"); if (!tok) continue; dt = parse_hex_byte(tok);
        tok = strtok(NULL, " \t\r\n"); if (!tok) continue; delay = parse_hex_byte(tok);
        tok = strtok(NULL, " \t\r\n"); if (!tok) continue; plen = parse_hex_byte(tok);
        if (dt < 0 || delay < 0 || plen <= 0 || plen > 200) {
            fprintf(stderr, "line %d: bad header\n", lineno);
            continue;
        }
        if (len + 4 + plen > cap) {
            cap *= 2;
            buf = realloc(buf, cap);
            if (!buf) { fclose(f); return -1; }
        }
        buf[len++] = (unsigned char)dt;
        buf[len++] = (unsigned char)delay;
        buf[len++] = (unsigned char)plen;
        buf[len++] = (unsigned char)(proto->use_lpm ? 1 : 0);  /* bit0: 1=LP */

        for (i = 0; i < plen; i++) {
            tok = strtok(NULL, " \t\r\n");
            if (!tok || parse_hex_byte(tok) < 0) {
                fprintf(stderr, "line %d: bad payload\n", lineno);
                len -= 4;
                plen = -1;
                break;
            }
            buf[len++] = (unsigned char)parse_hex_byte(tok);
        }
        if (plen < 0) continue;
        count++;
    }
    fclose(f);

    snprintf(dev_path, sizeof(dev_path), "/dev/mipi_dsi%d", proto->dev_num);
    fd = open(dev_path, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "open %s failed: %s (%d)\n", dev_path, strerror(errno), errno);
        free(buf);
        return -1;
    }

    req.count = count;
    req.len = len;
    req.data = (unsigned long long)(uintptr_t)buf;

    ret = ioctl(fd, DSI2_IOC_BATCH, &req);
    if (ret < 0) {
        fprintf(stderr, "ioctl batch failed: %s (%d), sent=%d\n",
                strerror(errno), errno, count);
        close(fd);
        free(buf);
        return -1;
    }
    printf("ioctl batch done: %d commands, %zu bytes, ret=%d\n", count, len, ret);
    close(fd);
    free(buf);
    return 0;
}

void print_usage() {
    printf("Usage:\n");
    printf("  Write: vismpwr [-0|-1|-b] [-lp|-hs] data_type delay payload_len payload...\n");
    printf("    e.g. vismpwr -hs 29 00 03 51 12 34\n");
    printf("    e.g. vismpwr -b -hs 29 00 03 51 12 34\n");
    printf("    default write mode is LP; add -hs to send in HS mode\n");
    printf("  Batch: vismpwr [-0|-1] [-lp|-hs] -f <file>   (one command per line, write())\n");
    printf("  Batch: vismpwr [-0|-1] [-lp|-hs] -i <file>   (one command per line, ioctl)\n");
    printf("  Read: vismpwr -r [-0|-1] [-lp|-hs] read_len read_reg\n");
    printf("    read does not support -b/--both\n");
    printf("    e.g. vismpwr -r 02 51          (LP read)\n");
    printf("    e.g. vismpwr -r -hs 02 51      (HS read for video mode)\n");
}

void print_version() {
    printf("vismpwr 2026-05-07\n");
}

int main(int argc, char *argv[]) {
    struct command cmd;
    int ret;
    const char *batch_file = NULL;
    const char *ioctl_file = NULL;
    int i;

    for (i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "-f") == 0) {
            batch_file = argv[i + 1];
            break;
        }
        if (strcmp(argv[i], "-i") == 0) {
            ioctl_file = argv[i + 1];
            break;
        }
    }

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
    if (cmd.batch == 2 && ioctl_file)
        return batch_ioctl_from_file(ioctl_file, &cmd) == 0 ? 0 : 1;
    if (batch_file)
        return batch_from_file(batch_file, &cmd) == 0 ? 0 : 1;

    return cmd.is_read ? read_register(&cmd) : write_register(&cmd);
}
