/*
 * Copyright (c) 2011 Peter Zotov <whitequark@whitequark.org>
 * Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.
 */

#include <ctype.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/types.h>

#if defined(_MSC_VER)
#include <stdbool.h>
#define __attribute__(x)
#endif

#if defined(_WIN32)
#include <win32_socket.h>
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

#include <stlink.h>
#include "gdb-server.h"
#include "gdb-remote.h"
#include "memory-map.h"
#include "semihosting.h"

#include <chipid.h>
#include <common_flash.h>
#include <flash_loader.h>
#include <helper.h>
#include <logging.h>
#include <read_write.h>
#include <register.h>
#include <usb.h>

#define FLASH_BASE 0x08000000

// Semihosting doesn't have a short option, we define a value to identify it
#define SEMIHOSTING_OPTION 128
#define SERIAL_OPTION 127

// always update the FLASH_PAGE before each use, by calling stlink_calculate_pagesize
#define FLASH_PAGE (sl->flash_pgsz)

static stlink_t *connected_stlink = NULL;

#if defined(_WIN32)
#define close_socket win32_close_socket
#define IS_SOCK_VALID(__sock) ((__sock) != INVALID_SOCKET)
#else
#define close_socket close
#define SOCKET int
#define IS_SOCK_VALID(__sock) ((__sock) > 0)
#endif

static const char hex[] = "0123456789abcdef";

typedef struct _st_state_t {
    // things from command line, bleh
    int32_t logging_level;
    int32_t listen_port;
    int32_t persistent;
    enum connect_type connect_mode;
    int32_t freq;
    char serialnumber[STLINK_SERIAL_BUFFER_SIZE];
    bool semihosting;
    const char* current_memory_map;
} st_state_t;


int32_t serve(stlink_t *sl, st_state_t *st);
char* make_memory_map(stlink_t *sl);
static void init_cache(stlink_t *sl);

static void _cleanup() {
    if (connected_stlink) {
        // Switch back to mass storage mode before closing
        stlink_run(connected_stlink, RUN_NORMAL);
        stlink_exit_debug_mode(connected_stlink);
        stlink_close(connected_stlink);
    }
}

static void cleanup(int32_t signum) {
    printf("Receive signal %i. Exiting...\n", signum);
    _cleanup();
    exit(1);
    (void)signum;
}

#if defined(_WIN32)
BOOL WINAPI CtrlHandler(DWORD fdwCtrlType) {
    printf("Receive signal %i. Exiting...\r\n", (int32_t)fdwCtrlType);
    _cleanup();
    return FALSE;
}
#endif

int32_t parse_options(int32_t argc, char** argv, st_state_t *st) {
    static struct option long_options[] = {
        {"help", no_argument, NULL, 'h'},
        {"verbose", optional_argument, NULL, 'v'},
        {"listen_port", required_argument, NULL, 'p'},
        {"multi", optional_argument, NULL, 'm'},
        {"no-reset", optional_argument, NULL, 'n'},
        {"hot-plug", optional_argument, NULL, 'n'},
        {"connect-under-reset", optional_argument, NULL, 'u'},
        {"freq", optional_argument, NULL, 'F'},
        {"version", no_argument, NULL, 'V'},
        {"semihosting", no_argument, NULL, SEMIHOSTING_OPTION},
        {"serial", required_argument, NULL, SERIAL_OPTION},
        {0, 0, 0, 0},
    };
    const char * help_str = "%s - usage:\n\n"
                            "  -h, --help\t\tPrint this help\n"
                            "  -V, --version\t\tPrint the version\n"
                            "  -vXX, --verbose=XX\tSpecify a specific verbosity level (0...99)\n"
                            "  -v, --verbose\t\tSpecify generally verbose logging\n"
                            "  -p 4242, --listen_port=1234\n"
                            "\t\t\tSet the gdb server listen port. "
                            "(default port: " STRINGIFY(DEFAULT_GDB_LISTEN_PORT) ")\n"
                            "  -m, --multi\n"
                            "\t\t\tSet gdb server to extended mode.\n"
                            "\t\t\tst-util will continue listening for connections after disconnect.\n"
                            "  -n, --no-reset, --hot-plug\n"
                            "\t\t\tDo not reset board on connection.\n"
                            "  -u, --connect-under-reset\n"
                            "\t\t\tConnect to the board before executing any instructions.\n"
                            "  -F 1800k, --freq=1M\n"
                            "\t\t\tSet the frequency of the SWD/JTAG interface.\n"
                            "  --semihosting\n"
                            "\t\t\tEnable semihosting support.\n"
                            "  --serial <serial>\n"
                            "\t\t\tUse a specific serial number.\n"
                            "\n"
                            "The STLINK device to use can be specified in the environment\n"
                            "variable STLINK_DEVICE on the format <USB_BUS>:<USB_ADDR>.\n"
                            "\n"
    ;


    int32_t option_index = 0;
    int32_t c;
    int32_t q;

    while ((c = getopt_long(argc, argv, "hv::p:mnu", long_options, &option_index)) != -1)
        switch (c) {
        case 0:
            break;
        case 'h':
            printf(help_str, argv[0]);
            exit(EXIT_SUCCESS);
            break;
        case 'v':
            if (optarg) {
                st->logging_level = atoi(optarg);
            } else {
                st->logging_level = DEBUG_LOGGING_LEVEL;
            }

            break;
        case 'p':
            if (sscanf(optarg, "%i", &q) != 1) {
                fprintf(stderr, "Invalid port %s\n", optarg);
                exit(EXIT_FAILURE);
            } else if (q < 0) {
                fprintf(stderr, "Can't use a negative port to listen on: %d\n", q);
                exit(EXIT_FAILURE);
            }

            st->listen_port = q;
            break;

        case 'm':
            st->persistent = true;
            break;
        case 'n':
            st->connect_mode = CONNECT_HOT_PLUG;
            break;
        case 'u':
            st->connect_mode = CONNECT_UNDER_RESET;
            break;
        case 'F':
            st->freq = arg_parse_freq(optarg);
            if (st->freq < 0) {
                fprintf(stderr, "Can't parse a frequency: %s\n", optarg);
                exit(EXIT_FAILURE);
            }
            break;
        case 'V':
            printf("v%s\n", STLINK_VERSION);
            exit(EXIT_SUCCESS);
        case SEMIHOSTING_OPTION:
            st->semihosting = true;
            break;
        case SERIAL_OPTION:
            printf("use serial %s\n", optarg);
            memcpy(st->serialnumber, optarg, STLINK_SERIAL_BUFFER_SIZE);
            break;
        }


    if (optind < argc) {
        printf("non-option ARGV-elements: ");

        while (optind < argc) { printf("%s ", argv[optind++]); }

        printf("\n");
    }

    return (0);
}

int32_t main(int32_t argc, char** argv) {
    stlink_t *sl = NULL;
    st_state_t state;
    memset(&state, 0, sizeof(state));

    // set defaults ...
    state.logging_level = DEFAULT_LOGGING_LEVEL;
    state.listen_port = DEFAULT_GDB_LISTEN_PORT;
    state.connect_mode = CONNECT_NORMAL; // by default, reset board
    parse_options(argc, argv, &state);

    printf("st-util %s\n", STLINK_VERSION);

    init_chipids (STLINK_CHIPS_DIR);

    sl = stlink_open_usb(state.logging_level, state.connect_mode, state.serialnumber, state.freq);
    if (sl == NULL) { return (1); }

    if (sl->chip_id == STM32_CHIPID_UNKNOWN) {
        ELOG("Unsupported Target (Chip ID is %#010x, Core ID is %#010x).\n", sl->chip_id, sl->core_id);
        return (1);
    }

    sl->verbose = 0;
    connected_stlink = sl;

#if defined(_WIN32)
    SetConsoleCtrlHandler((PHANDLER_ROUTINE) CtrlHandler, TRUE);
#else
    signal(SIGINT, &cleanup);
    signal(SIGTERM, &cleanup);
    signal(SIGSEGV, &cleanup);
#endif

    DLOG("Chip ID is %#010x, Core ID is %#08x.\n", sl->chip_id, sl->core_id);

#if defined(_WIN32)
    WSADATA wsadata;

    if (WSAStartup(MAKEWORD(2, 2), &wsadata) != 0) { goto winsock_error; }
#endif

    do {                            // don't go beserk if serve() returns with error
        if (serve(sl, &state)) { usleep (1 * 1000); }

        sl = connected_stlink;      // in case serve() changed the connection
        stlink_run(sl, RUN_NORMAL); // continue
    } while (state.persistent);

#if defined(_WIN32)
winsock_error:
    WSACleanup();
#endif

    // switch back to mass storage mode before closing
    stlink_exit_debug_mode(sl);
    stlink_close(sl);

    return (0);
}

static const char* const target_description =
    "<?xml version=\"1.0\"?>"
    "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
    "<target version=\"1.0\">"
    "   <architecture>arm</architecture>"
    "   <feature name=\"org.gnu.gdb.arm.m-profile\">"
    "       <reg name=\"r0\" bitsize=\"32\"/>"
    "       <reg name=\"r1\" bitsize=\"32\"/>"
    "       <reg name=\"r2\" bitsize=\"32\"/>"
    "       <reg name=\"r3\" bitsize=\"32\"/>"
    "       <reg name=\"r4\" bitsize=\"32\"/>"
    "       <reg name=\"r5\" bitsize=\"32\"/>"
    "       <reg name=\"r6\" bitsize=\"32\"/>"
    "       <reg name=\"r7\" bitsize=\"32\"/>"
    "       <reg name=\"r8\" bitsize=\"32\"/>"
    "       <reg name=\"r9\" bitsize=\"32\"/>"
    "       <reg name=\"r10\" bitsize=\"32\"/>"
    "       <reg name=\"r11\" bitsize=\"32\"/>"
    "       <reg name=\"r12\" bitsize=\"32\"/>"
    "       <reg name=\"sp\" bitsize=\"32\" type=\"data_ptr\"/>"
    "       <reg name=\"lr\" bitsize=\"32\"/>"
    "       <reg name=\"pc\" bitsize=\"32\" type=\"code_ptr\"/>"
    "       <reg name=\"xpsr\" bitsize=\"32\" regnum=\"25\"/>"
    "       <reg name=\"msp\" bitsize=\"32\" regnum=\"26\" type=\"data_ptr\" group=\"general\" />"
    "       <reg name=\"psp\" bitsize=\"32\" regnum=\"27\" type=\"data_ptr\" group=\"general\" />"
    "       <reg name=\"control\" bitsize=\"8\" regnum=\"28\" type=\"int\" group=\"general\" />"
    "       <reg name=\"faultmask\" bitsize=\"8\" regnum=\"29\" type=\"int\" group=\"general\" />"
    "       <reg name=\"basepri\" bitsize=\"8\" regnum=\"30\" type=\"int\" group=\"general\" />"
    "       <reg name=\"primask\" bitsize=\"8\" regnum=\"31\" type=\"int\" group=\"general\" />"
    "       <reg name=\"s0\" bitsize=\"32\" regnum=\"32\" type=\"float\" group=\"float\" />"
    "       <reg name=\"s1\" bitsize=\"32\" type=\"float\" group=\"float\" />"
    "       <reg name=\"s2\" bitsize=\"32\" type=\"float\" group=\"float\" />"
    "       <reg name=\"s3\" bitsize=\"32\" type=\"float\" group=\"float\" />"
    "       <reg name=\"s4\" bitsize=\"32\" type=\"float\" group=\"float\" />"
    "       <reg name=\"s5\" bitsize=\"32\" type=\"float\" group=\"float\" />"
    "       <reg name=\"s6\" bitsize=\"32\" type=\"float\" group=\"float\" />"
    "       <reg name=\"s7\" bitsize=\"32\" type=\"float\" group=\"float\" />"
    "       <reg name=\"s8\" bitsize=\"32\" type=\"float\" group=\"float\" />"
    "       <reg name=\"s9\" bitsize=\"32\" type=\"float\" group=\"float\" />"
    "       <reg name=\"s10\" bitsize=\"32\" type=\"float\" group=\"float\" />"
    "       <reg name=\"s11\" bitsize=\"32\" type=\"float\" group=\"float\" />"
    "       <reg name=\"s12\" bitsize=\"32\" type=\"float\" group=\"float\" />"
    "       <reg name=\"s13\" bitsize=\"32\" type=\"float\" group=\"float\" />"
    "       <reg name=\"s14\" bitsize=\"32\" type=\"float\" group=\"float\" />"
    "       <reg name=\"s15\" bitsize=\"32\" type=\"float\" group=\"float\" />"
    "       <reg name=\"s16\" bitsize=\"32\" type=\"float\" group=\"float\" />"
    "       <reg name=\"s17\" bitsize=\"32\" type=\"float\" group=\"float\" />"
    "       <reg name=\"s18\" bitsize=\"32\" type=\"float\" group=\"float\" />"
    "       <reg name=\"s19\" bitsize=\"32\" type=\"float\" group=\"float\" />"
    "       <reg name=\"s20\" bitsize=\"32\" type=\"float\" group=\"float\" />"
    "       <reg name=\"s21\" bitsize=\"32\" type=\"float\" group=\"float\" />"
    "       <reg name=\"s22\" bitsize=\"32\" type=\"float\" group=\"float\" />"
    "       <reg name=\"s23\" bitsize=\"32\" type=\"float\" group=\"float\" />"
    "       <reg name=\"s24\" bitsize=\"32\" type=\"float\" group=\"float\" />"
    "       <reg name=\"s25\" bitsize=\"32\" type=\"float\" group=\"float\" />"
    "       <reg name=\"s26\" bitsize=\"32\" type=\"float\" group=\"float\" />"
    "       <reg name=\"s27\" bitsize=\"32\" type=\"float\" group=\"float\" />"
    "       <reg name=\"s28\" bitsize=\"32\" type=\"float\" group=\"float\" />"
    "       <reg name=\"s29\" bitsize=\"32\" type=\"float\" group=\"float\" />"
    "       <reg name=\"s30\" bitsize=\"32\" type=\"float\" group=\"float\" />"
    "       <reg name=\"s31\" bitsize=\"32\" type=\"float\" group=\"float\" />"
    "       <reg name=\"fpscr\" bitsize=\"32\" type=\"int\" group=\"float\" />"
    "   </feature>"
    "</target>";

char* make_memory_map(stlink_t *sl) {
    // this will be freed in serve()
    const uint32_t sz = 4096;
    char* map = malloc(sz);
    map[0] = '\0';

    if (sl->chip_id == STM32_CHIPID_F4 ||
        sl->chip_id == STM32_CHIPID_F446 ||
        sl->chip_id == STM32_CHIPID_F411xx) {
            strcpy(map, memory_map_template_F4);
    } else if (sl->chip_id == STM32_CHIPID_F4_DE) {
        strcpy(map, memory_map_template_F4_DE);
    } else if (sl->core_id == STM32_CORE_ID_M7F_SWD) {
        snprintf(map, sz, memory_map_template_F7,
                 sl->sram_size);
    } else if (sl->chip_id == STM32_CHIPID_H74xxx) {
        snprintf(map, sz, memory_map_template_H7,
                 sl->flash_size,
                 sl->flash_pgsz);
    } else if (sl->chip_id == STM32_CHIPID_F4_HD) {
        strcpy(map, memory_map_template_F4_HD);
    } else if (sl->chip_id == STM32_CHIPID_F2) {
        snprintf(map, sz, memory_map_template_F2,
                 sl->flash_size,
                 sl->sram_size,
                 sl->flash_size - 0x20000,
                 sl->sys_base,
                 sl->sys_size);
    } else if ((sl->chip_id == STM32_CHIPID_L4) ||
               (sl->chip_id == STM32_CHIPID_L43x_L44x) ||
               (sl->chip_id == STM32_CHIPID_L45x_L46x)) {
        snprintf(map, sz, memory_map_template_L4,
                 sl->flash_size,
                 sl->flash_size);
    } else if (sl->chip_id == STM32_CHIPID_L496x_L4A6x) {
        snprintf(map, sz, memory_map_template_L496,
                 sl->flash_size,
                 sl->flash_size);
    } else if (sl->chip_id == STM32_CHIPID_H72x) {
        snprintf(map, sz, memory_map_template_H72x3x,
                 sl->flash_size,
                 sl->flash_pgsz);
	} else {
        snprintf(map, sz, memory_map_template,
                 sl->flash_size,
                 sl->sram_size,
                 sl->flash_size,
                 sl->flash_pgsz,
                 sl->sys_base,
                 sl->sys_size);
    }

    return (map);
}

#define DATA_WATCH_NUM 4

enum watchfun { WATCHDISABLED = 0, WATCHREAD = 5, WATCHWRITE = 6, WATCHACCESS = 7 };

struct code_hw_watchpoint {
    stm32_addr_t addr;
    uint8_t mask;
    enum watchfun fun;
};

static struct code_hw_watchpoint data_watches[DATA_WATCH_NUM];

static void init_data_watchpoints(stlink_t *sl) {
    uint32_t data;
    DLOG("init watchpoints\n");

    // set TRCENA in debug command to turn on DWT unit
    stlink_read_debug32(sl, STLINK_REG_CM3_DEMCR, &data);
    data |= STLINK_REG_CM3_DEMCR_TRCENA;
    stlink_write_debug32(sl, STLINK_REG_CM3_DEMCR, data);

    // make sure all watchpoints are cleared
    for (int32_t i = 0; i < DATA_WATCH_NUM; i++) {
        data_watches[i].fun = WATCHDISABLED;
        stlink_write_debug32(sl, STLINK_REG_CM3_DWT_FUNn(i), 0);
    }
}

static int32_t add_data_watchpoint(stlink_t *sl, enum watchfun wf, stm32_addr_t addr, uint32_t len) {
    int32_t i = 0;
    uint32_t mask, dummy;

    // computer mask
    // find a free watchpoint
    // configure

    mask = -1;
    i = len;

    while (i) {
        i >>= 1;
        mask++;
    }

    if ((mask != (uint32_t)-1) && (mask < 16)) {
        for (i = 0; i < DATA_WATCH_NUM; i++)
            // is this an empty slot ?
            if (data_watches[i].fun == WATCHDISABLED) {
                DLOG("insert watchpoint %d addr %x wf %u mask %u len %d\n", i, addr, wf, mask, len);

                data_watches[i].fun = wf;
                data_watches[i].addr = addr;
                data_watches[i].mask = mask;

                // insert comparator address
                stlink_write_debug32(sl, STLINK_REG_CM3_DWT_COMPn(i), addr);

                // insert mask
                stlink_write_debug32(sl, STLINK_REG_CM3_DWT_MASKn(i), mask);

                // insert function
                stlink_write_debug32(sl, STLINK_REG_CM3_DWT_FUNn(i), wf);

                // just to make sure the matched bit is clear !
                stlink_read_debug32(sl,  STLINK_REG_CM3_DWT_FUNn(i), &dummy);
                return (0);
            }
    }

    DLOG("failure: add watchpoints addr %x wf %u len %u\n", addr, wf, len);
    return (-1);
}

static int32_t delete_data_watchpoint(stlink_t *sl, stm32_addr_t addr) {
    int32_t i;

    for (i = 0; i < DATA_WATCH_NUM; i++) {
        if ((data_watches[i].addr == addr) && (data_watches[i].fun != WATCHDISABLED)) {
            DLOG("delete watchpoint %d addr %x\n", i, addr);

            data_watches[i].fun = WATCHDISABLED;
            stlink_write_debug32(sl, STLINK_REG_CM3_DWT_FUNn(i), 0);

            return (0);
        }
    }

    DLOG("failure: delete watchpoint addr %x\n", addr);

    return (-1);
}

static int32_t code_break_num;
static int32_t code_lit_num;
static int32_t code_break_rev;
#define CODE_BREAK_NUM_MAX 15
#define CODE_BREAK_LOW     0x01
#define CODE_BREAK_HIGH    0x02
#define CODE_BREAK_REMAP   0x04
#define CODE_BREAK_REV_V1  0x00
#define CODE_BREAK_REV_V2  0x01

struct code_hw_breakpoint {
    stm32_addr_t addr;
    int32_t type;
};

static struct code_hw_breakpoint code_breaks[CODE_BREAK_NUM_MAX];

static void init_code_breakpoints(stlink_t *sl) {
    uint32_t val;
    memset(sl->q_buf, 0, 4);
    stlink_write_debug32(sl, STLINK_REG_CM3_FP_CTRL, 0x03 /* KEY | ENABLE */);
    stlink_read_debug32(sl, STLINK_REG_CM3_FP_CTRL, &val);
    code_break_num = ((val >> 4) & 0xf);
    code_lit_num = ((val >> 8) & 0xf);
    code_break_rev = ((val >> 28) & 0xf);

    ILOG("Found %i hw breakpoint registers\n", code_break_num);

    stlink_read_debug32(sl, STLINK_REG_CM3_CPUID, &val);
    if (((val>>4) & 0xFFF) == 0xC27) {
        // Cortex-M7 can have locked to write FP_* registers
        // IHI0029D, p. 48, Lock Access Register
        stlink_write_debug32(sl, STLINK_REG_CM7_FP_LAR, STLINK_REG_CM7_FP_LAR_KEY);
    }

    for (int32_t i = 0; i < code_break_num; i++) {
        code_breaks[i].type = 0;
        stlink_write_debug32(sl, STLINK_REG_CM3_FP_COMPn(i), 0);
    }
}

static int32_t has_breakpoint(stm32_addr_t addr) {
    for (int32_t i = 0; i < code_break_num; i++)
        if (code_breaks[i].addr == addr) { return (1); }

    return (0);
}

static int32_t update_code_breakpoint(stlink_t *sl, stm32_addr_t addr, int32_t set) {
    uint32_t mask;
    int32_t type;
    stm32_addr_t fpb_addr;

    if (addr & 1) {
        ELOG("update_code_breakpoint: unaligned address %08x\n", addr);
        return (-1);
    }

    if (code_break_rev == CODE_BREAK_REV_V1) {
        type = (addr & 0x2) ? CODE_BREAK_HIGH : CODE_BREAK_LOW;
        fpb_addr = addr & 0x1FFFFFFC;
    } else {
        type = CODE_BREAK_REMAP;
        fpb_addr = addr;
    }

    int32_t id = -1;
    for (int32_t i = 0; i < code_break_num; i++)
        if (fpb_addr == code_breaks[i].addr || (set && code_breaks[i].type == 0)) {
            id = i;
            break;
        }

    if (id == -1) {
        if (set)
            return (-1); // free slot not found
        else
            return (0); // breakpoint is already removed
    }

    struct code_hw_breakpoint* bp = &code_breaks[id];
    bp->addr = fpb_addr;
    if (set)
        bp->type |= type;
    else
        bp->type &= ~type;

    // DDI0403E, p. 759, FP_COMPn register description
    mask = ((bp->type&0x03) << 30) | bp->addr | 1;

    if (bp->type == 0) {
        DLOG("clearing hw break %d\n", id);
        stlink_write_debug32(sl, STLINK_REG_CM3_FP_COMPn(id), 0);
    } else {
        DLOG("setting hw break %d at %08x (%d)\n", id, bp->addr, bp->type);
        DLOG("reg %08x \n", mask);
        stlink_write_debug32(sl, STLINK_REG_CM3_FP_COMPn(id), mask);
    }

    return (0);
}


struct flash_block {
    stm32_addr_t addr;
    uint32_t length;
    uint8_t*     data;

    struct flash_block* next;
};

static struct flash_block* flash_root;

static int32_t flash_add_block(stm32_addr_t addr, uint32_t length, stlink_t *sl) {

    if (addr < FLASH_BASE || addr + length > FLASH_BASE + sl->flash_size) {
        ELOG("flash_add_block: incorrect bounds\n");
        return (-1);
    }

    stlink_calculate_pagesize(sl, addr);

    if (addr % FLASH_PAGE != 0 || length % FLASH_PAGE != 0) {
        ELOG("flash_add_block: unaligned block\n");
        return (-1);
    }

    struct flash_block* new = malloc(sizeof(struct flash_block));
    new->next   = flash_root;
    new->addr   = addr;
    new->length = length;
    new->data   = malloc(length);
    memset(new->data, stlink_get_erased_pattern(sl), length);

    flash_root = new;
    return (0);
}

static int32_t flash_populate(stm32_addr_t addr, uint8_t* data, uint32_t length) {
    uint32_t fit_blocks = 0, fit_length = 0;

    for (struct flash_block* fb = flash_root; fb; fb = fb->next) {
        /*
         * Block: ------X------Y--------
         * Data:            a-----b
         *                a--b
         *            a-----------b
         * Block intersects with data, if:
         *  a < Y && b > x
         */

        uint32_t X = fb->addr, Y = fb->addr + fb->length;
        uint32_t a = addr, b = addr + length;

        if (a < Y && b > X) {
            // from start of the block
            uint32_t start = (a > X ? a : X) - X;
            uint32_t end   = (b > Y ? Y : b) - X;

            memcpy(fb->data + start, data, end - start);

            fit_blocks++;
            fit_length += end - start;
        }
    }

    if (fit_blocks == 0) {
        ELOG("Unfit data block %08x -> %04x\n", addr, length);
        return (-1);
    }

    if (fit_length != length) {
        WLOG("data block %08x -> %04x truncated to %04x\n", addr, length, fit_length);
        WLOG("(this is not an error, just a GDB glitch)\n");
    }

    return (0);
}

static int32_t flash_go(stlink_t *sl, st_state_t *st) {
    int32_t error = -1;
    int32_t ret;
    flash_loader_t fl;

    stlink_target_connect(sl, st->connect_mode);
    stlink_force_debug(sl);

    for (struct flash_block* fb = flash_root; fb; fb = fb->next) {
        ILOG("flash_erase: block %08x -> %04x\n", fb->addr, fb->length);

        for (stm32_addr_t page = fb->addr; page < fb->addr + fb->length; page += (uint32_t)FLASH_PAGE) {
            // update FLASH_PAGE
            stlink_calculate_pagesize(sl, page);

            ILOG("flash_erase: page %08x\n", page);
            ret = stlink_erase_flash_page(sl, page);
            if (ret) { goto error; }
        }
    }

    ret = stlink_flashloader_start(sl, &fl);
    if (ret) { goto error; }

    for (struct flash_block* fb = flash_root; fb; fb = fb->next) {
        ILOG("flash_do: block %08x -> %04x\n", fb->addr, fb->length);

        for (stm32_addr_t page = fb->addr; page < fb->addr + fb->length; page += (uint32_t)FLASH_PAGE) {
            uint32_t length = fb->length - (page - fb->addr);

            // update FLASH_PAGE
            stlink_calculate_pagesize(sl, page);

            ILOG("flash_do: page %08x\n", page);
            uint32_t len = (length > FLASH_PAGE) ? (uint32_t)FLASH_PAGE : length;
            ret = stlink_flashloader_write(sl, &fl, page, fb->data + (page - fb->addr), len);
            if (ret) { goto error; }
        }
    }

    stlink_flashloader_stop(sl, &fl);
    stlink_reset(sl, RESET_SOFT_AND_HALT);
    error = 0;

error:

    for (struct flash_block* fb = flash_root, *next; fb; fb = next) {
        next = fb->next;
        free(fb->data);
        free(fb);
    }

    flash_root = NULL;
    return (error);
}

struct cache_level_desc {
    uint32_t nsets;
    uint32_t nways;
    uint32_t log2_nways;
    uint32_t width;
};

struct cache_desc_t {
    uint32_t used;

    // minimal line size in bytes
    uint32_t dminline;
    uint32_t iminline;

    // last level of unification (uniprocessor)
    uint32_t louu;

    struct cache_level_desc icache[7];
    struct cache_level_desc dcache[7];
};

static struct cache_desc_t cache_desc;

// return the smallest R so that V <= (1 << R); not performance critical
static uint32_t ceil_log2(uint32_t v) {
    uint32_t res;

    for (res = 0; (1U << res) < v; res++);

    return (res);
}

static void read_cache_level_desc(stlink_t *sl, struct cache_level_desc *desc) {
    uint32_t ccsidr;
    uint32_t log2_nsets;

    stlink_read_debug32(sl, STLINK_REG_CM7_CCSIDR, &ccsidr);
    desc->nsets = ((ccsidr >> 13) & 0x3fff) + 1;
    desc->nways = ((ccsidr >> 3) & 0x1ff) + 1;
    desc->log2_nways = ceil_log2 (desc->nways);
    log2_nsets = ceil_log2 (desc->nsets);
    desc->width = 4 + (ccsidr & 7) + log2_nsets;
    ILOG("%08x LineSize: %u, ways: %u, sets: %u (width: %u)\n",
         ccsidr, 4 << (ccsidr & 7), desc->nways, desc->nsets, desc->width);
}

static void init_cache (stlink_t *sl) {
    uint32_t clidr;
    uint32_t ccr;
    uint32_t ctr;
    int32_t i;

    // Check have cache
    stlink_read_debug32(sl, STLINK_REG_CM7_CTR, &ctr);
    if ((ctr >> 29) != 0x04) {
        cache_desc.used = 0;
        return;
    } else
        cache_desc.used = 1;
    cache_desc.dminline = 4 << ((ctr >> 16) & 0x0f);
    cache_desc.iminline = 4 << (ctr & 0x0f);

    stlink_read_debug32(sl, STLINK_REG_CM7_CLIDR, &clidr);
    cache_desc.louu = (clidr >> 27) & 7;

    stlink_read_debug32(sl, STLINK_REG_CM7_CCR, &ccr);
    ILOG("Chip clidr: %08x, I-Cache: %s, D-Cache: %s\n",
         clidr, ccr & STLINK_REG_CM7_CCR_IC ? "on" : "off", ccr & STLINK_REG_CM7_CCR_DC ? "on" : "off");
    ILOG(" cache: LoUU: %u, LoC: %u, LoUIS: %u\n",
         (clidr >> 27) & 7, (clidr >> 24) & 7, (clidr >> 21) & 7);
    ILOG(" cache: ctr: %08x, DminLine: %u bytes, IminLine: %u bytes\n", ctr,
         cache_desc.dminline, cache_desc.iminline);

    for (i = 0; i < 7; i++) {
        uint32_t ct = (clidr >> (3 * i)) & 0x07;
        cache_desc.dcache[i].width = 0;
        cache_desc.icache[i].width = 0;

        if (ct == 2 || ct == 3 || ct == 4) { // data
            stlink_write_debug32(sl, STLINK_REG_CM7_CSSELR, i << 1);
            ILOG("D-Cache L%d: ", i);
            read_cache_level_desc(sl, &cache_desc.dcache[i]);
        }

        if (ct == 1 || ct == 3) { // instruction
            stlink_write_debug32(sl, STLINK_REG_CM7_CSSELR, (i << 1) | 1);
            ILOG("I-Cache L%d: ", i);
            read_cache_level_desc(sl, &cache_desc.icache[i]);
        }
    }
}

static void cache_flush(stlink_t *sl, uint32_t ccr) {
    int32_t level;

    if (ccr & STLINK_REG_CM7_CCR_DC) {
        for (level = cache_desc.louu - 1; level >= 0; level--) {
            struct cache_level_desc *desc = &cache_desc.dcache[level];
            uint32_t addr;
            uint32_t max_addr = 1 << desc->width;
            uint32_t way_sh = 32 - desc->log2_nways;

            // D-cache clean by set-ways.
            for (addr = (level << 1); addr < max_addr; addr += cache_desc.dminline) {
                uint32_t way;

                for (way = 0; way < desc->nways; way++) {
                    stlink_write_debug32(sl, STLINK_REG_CM7_DCCSW, addr | (way << way_sh));
                }
            }
        }
    }

    // invalidate all I-cache to oPU
    if (ccr & STLINK_REG_CM7_CCR_IC) {
        stlink_write_debug32(sl, STLINK_REG_CM7_ICIALLU, 0);
    }
}

static int32_t cache_modified;

static void cache_change(stm32_addr_t start, uint32_t count) {
    if (count == 0) { return; }

    (void)start;
    cache_modified = 1;
}

static void cache_sync(stlink_t *sl) {
    uint32_t ccr;

    if (!cache_desc.used) { return; }

    if (!cache_modified) { return; }

    cache_modified = 0;
    stlink_read_debug32(sl, STLINK_REG_CM7_CCR, &ccr);
    if (ccr & (STLINK_REG_CM7_CCR_IC | STLINK_REG_CM7_CCR_DC)) { cache_flush(sl, ccr); }
}

static uint32_t unhexify(const char *in, char *out, uint32_t out_count) {
    uint32_t i;
    uint32_t c;

    for (i = 0; i < out_count; i++) {
        if (sscanf(in + (2 * i), "%02x", &c) != 1) { return (i); }

        out[i] = (char)c;
    }

    return (i);
}

int32_t serve(stlink_t *sl, st_state_t *st) {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);

    if (!IS_SOCK_VALID(sock)) {
        perror("socket");
        return (1);
    }

    uint32_t val = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *)&val, sizeof(val));

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(struct sockaddr_in));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(st->listen_port);

    if (bind(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("bind");
        close_socket(sock);
        return (1);
    }

    if (listen(sock, 5) < 0) {
        perror("listen");
        close_socket(sock);
        return (1);
    }

    ILOG("Listening at *:%d...\n", st->listen_port);

    SOCKET client = accept(sock, NULL, NULL);

    // signal (SIGINT, SIG_DFL);
    if (!IS_SOCK_VALID(client)) {
        perror("accept");
        close_socket(sock);
        return (1);
    }

    close_socket(sock);

    uint32_t chip_id = sl->chip_id;

    stlink_target_connect(sl, st->connect_mode);
    stlink_force_debug(sl);

    if (sl->chip_id != chip_id) {
        WLOG("Target has changed!\n");
    }

    init_code_breakpoints(sl);
    init_data_watchpoints(sl);

    init_cache(sl);

    st->current_memory_map = make_memory_map(sl);

    ILOG("GDB connected.\n");

    /*
     * To allow resetting the chip from GDB it is required to emulate attaching
     * and detaching to target.
     */
    uint32_t attached = 1;
    // if a critical error is detected, break from the loop
    int32_t critical_error = 0;
    int32_t ret;

    while (1) {
        ret = 0;
        char* packet;

        int32_t status = gdb_recv_packet(client, &packet);

        if (status < 0) {
            ELOG("cannot recv: %d\n", status);
            close_socket(client);
            return (1);
        }

        DLOG("recv: %s\n", packet);

        char* reply = NULL;
        struct stlink_reg regp;

        switch (packet[0]) {
        case 'q': {
            if (packet[1] == 'P' || packet[1] == 'C' || packet[1] == 'L') {
                reply = strdup("");
                break;
            }

            char *separator = strstr(packet, ":"), *params = "";

            if (separator == NULL) {
                separator = packet + strlen(packet);
            } else {
                params = separator + 1;
            }

            uint32_t queryNameLength = (uint32_t)(separator - &packet[1]);
            char* queryName = calloc(queryNameLength + 1, 1);
            strncpy(queryName, &packet[1], queryNameLength);

            DLOG("query: %s;%s\n", queryName, params);

            if (!strcmp(queryName, "Supported")) {
                reply = strdup("PacketSize=3fff;qXfer:memory-map:read+;qXfer:features:read+");
            } else if (!strcmp(queryName, "Xfer")) {
                char *type, *op, *__s_addr, *s_length;
                char *tok = params;
                char *annex __attribute__((unused));

                type     = strsep(&tok, ":");
                op       = strsep(&tok, ":");
                annex    = strsep(&tok, ":");
                __s_addr = strsep(&tok, ",");
                s_length = tok;

                uint32_t addr = (uint32_t)strtoul(__s_addr, NULL, 16),
                         length = (uint32_t)strtoul(s_length, NULL, 16);

                DLOG("Xfer: type:%s;op:%s;annex:%s;addr:%d;length:%d\n",
                     type, op, annex, addr, length);

                const char* data;
                if (strcmp(op, "read")) {
                    data = NULL;
                } else if (!strcmp(type, "memory-map")) {
                    data = st->current_memory_map;
                } else if (!strcmp(type, "features")) {
                    data = target_description;
                } else {
                    data = NULL;
                }

                if (data) {
                    uint32_t data_length = (uint32_t)strlen(data);

                    if (addr + length > data_length) { length = data_length - addr; }

                    if (length == 0) {
                        reply = strdup("l");
                    } else {
                        reply = calloc(length + 2, 1);
                        reply[0] = 'm';
                        strncpy(&reply[1], data, length);
                    }
                }
            } else if (!strncmp(queryName, "Rcmd,", 4)) {
                // Rcmd uses the wrong separator
                separator = strstr(packet, ",");
                params = "";

                if (separator == NULL) {
                    separator = packet + strlen(packet);
                } else {
                    params = separator + 1;
                }

                uint32_t hex_len = (uint32_t)strlen(params);
                uint32_t alloc_size = (hex_len / 2) + 1;
                uint32_t cmd_len;
                char *cmd = malloc(alloc_size);

                if (cmd == NULL) {
                    DLOG("Rcmd unhexify allocation error\n");
                    break;
                }

                cmd_len = unhexify(params, cmd, alloc_size - 1);
                cmd[cmd_len] = 0;

                DLOG("unhexified Rcmd: '%s'\n", cmd);

                if (!strncmp(cmd, "resume", 6)) {                               // resume
                    DLOG("Rcmd: resume\n");
                    cache_sync(sl);
                    ret = stlink_run(sl, RUN_NORMAL);

                    if (ret) {
                        DLOG("Rcmd: resume failed\n");
                        reply = strdup("E00");
                    } else {
                        reply = strdup("OK");
                    }

                } else if (!strncmp(cmd, "halt", 4)) {                          // halt
                    ret = stlink_force_debug(sl);

                    if (ret) {
                        DLOG("Rcmd: halt failed\n");
                        reply = strdup("E00");
                    } else {
                        reply = strdup("OK");
                        DLOG("Rcmd: halt\n");
                    }

                } else if (!strncmp(cmd, "jtag_reset", 10)) {                   // jtag_reset
                    reply = strdup("OK");

                    ret = stlink_reset(sl, RESET_HARD);
                    if (ret) {
                        DLOG("Rcmd: jtag_reset failed with jtag_reset\n");
                        reply = strdup("E00");
                    }

                    ret = stlink_force_debug(sl);
                    if (ret) {
                        DLOG("Rcmd: jtag_reset failed with force_debug\n");
                        reply = strdup("E00");
                    }

                    if (strcmp(reply, "E00")) {
                        // no errors have been found
                        DLOG("Rcmd: jtag_reset\n");
                    }
                } else if (!strncmp(cmd, "reset", 5)) {     // reset

                    ret = stlink_force_debug(sl);
                    if (ret) {
                        DLOG("Rcmd: reset failed with force_debug\n");
                        reply = strdup("E00");
                    }

                    ret = stlink_reset(sl, RESET_SOFT_AND_HALT);
                    if (ret) {
                        DLOG("Rcmd: reset failed with reset\n");
                        reply = strdup("E00");
                    }

                    init_code_breakpoints(sl);
                    init_data_watchpoints(sl);

                    if (reply == NULL) {
                        reply = strdup("OK");
                        DLOG("Rcmd: reset\n");
                    }

                } else if (!strncmp(cmd, "semihosting ", 12)) {
                    DLOG("Rcmd: got semihosting cmd '%s'", cmd);
                    char *arg = cmd + 12;

                    while (isspace(*arg)) { arg++; } // skip whitespaces

                    if (!strncmp(arg, "enable", 6) || !strncmp(arg, "1", 1)) {
                        st->semihosting = true;
                        reply = strdup("OK");
                    } else if (!strncmp(arg, "disable", 7) || !strncmp(arg, "0", 1)) {
                        st->semihosting = false;
                        reply = strdup("OK");
                    } else {
                        DLOG("Rcmd: unknown semihosting arg: '%s'\n", arg);
                    }
                } else {
                    DLOG("Rcmd: %s\n", cmd);
                }

                free(cmd);
            }

            if (reply == NULL) { reply = strdup(""); }

            free(queryName);
            break;
        }

        case 'v': {
            char *params = NULL;
            char *cmdName = strtok_r(packet, ":;", &params);

            cmdName++; // vCommand -> Command

            if (!strcmp(cmdName, "FlashErase")) {
                char *__s_addr, *s_length;
                char *tok = params;

                __s_addr   = strsep(&tok, ",");
                s_length = tok;

                uint32_t addr = (uint32_t)strtoul(__s_addr, NULL, 16),
                         length = (uint32_t)strtoul(s_length, NULL, 16);

                DLOG("FlashErase: addr:%08x,len:%04x\n",
                     addr, length);

                if (flash_add_block(addr, length, sl) < 0) {
                    reply = strdup("E00");
                } else {
                    reply = strdup("OK");
                }
            } else if (!strcmp(cmdName, "FlashWrite")) {
                char *__s_addr, *data;
                char *tok = params;

                __s_addr = strsep(&tok, ":");
                data   = tok;

                uint32_t addr = (uint32_t)strtoul(__s_addr, NULL, 16);
                uint32_t data_length = status - (uint32_t)(data - packet);

                // Length of decoded data cannot be more than encoded, as escapes are removed.
                // Additional byte is reserved for alignment fix.
                uint8_t *decoded = calloc(data_length + 1, 1);
                uint32_t dec_index = 0;

                for (uint32_t i = 0; i < data_length; i++) {
                    if (data[i] == 0x7d) {
                        i++;
                        decoded[dec_index++] = data[i] ^ 0x20;
                    } else {
                        decoded[dec_index++] = data[i];
                    }
                }

                // fix alignment
                if (dec_index % 2 != 0) { dec_index++; }

                DLOG("binary packet %d -> %d\n", data_length, dec_index);

                if (flash_populate(addr, decoded, dec_index) < 0) {
                    reply = strdup("E00");
                } else {
                    reply = strdup("OK");
                }

                free(decoded);
            } else if (!strcmp(cmdName, "FlashDone")) {
                if (flash_go(sl, st)) {
                    reply = strdup("E08");
                } else {
                    reply = strdup("OK");
                }
            } else if (!strcmp(cmdName, "Kill")) {
                attached = 0;
                reply = strdup("OK");
            }

            if (reply == NULL) { reply = strdup(""); }

            break;
        }

        case 'c':
            cache_sync(sl);
            ret = stlink_run(sl, RUN_NORMAL);

            if (ret) { DLOG("Semihost: run failed\n"); }

            while (1) {
                status = gdb_check_for_interrupt(client);

                if (status < 0) {
                    ELOG("cannot check for int: %d\n", status);
                    close_socket(client);
                    return (1);
                }

                if (status == 1) {
                    stlink_force_debug(sl);
                    break;
                }

                ret = stlink_status(sl);

                if (ret) { DLOG("Semihost: status failed\n"); }

                if (sl->core_stat == TARGET_HALTED) {
                    struct stlink_reg reg;
                    stm32_addr_t pc;
                    stm32_addr_t addr;
                    int32_t offset = 0;
                    uint16_t insn;

                    if (!st->semihosting) { break; }

                    ret = stlink_read_all_regs (sl, &reg);

                    if (ret) { DLOG("Semihost: read_all_regs failed\n"); }

                    // read PC
                    pc = reg.r[15];

                    // compute aligned value
                    offset = pc % 4;
                    addr = pc - offset;

                    // read instructions (address and length must be aligned).
                    ret = stlink_read_mem32(sl, addr, (offset > 2 ? 8 : 4));

                    if (ret != 0) {
                        DLOG("Semihost: cannot read instructions at: 0x%08x\n", addr);
                        break;
                    }

                    memcpy(&insn, &sl->q_buf[offset], sizeof(insn));

                    if (insn == 0xBEAB && !has_breakpoint(addr)) {

                        ret = do_semihosting (sl, reg.r[0], reg.r[1], &reg.r[0]);

                        if (ret) { DLOG("Semihost: do_semihosting failed\n"); }

                        // write return value
                        ret = stlink_write_reg(sl, reg.r[0], 0);

                        if (ret) { DLOG("Semihost: write_reg failed for return value\n"); }

                        // jump over the break instruction
                        ret = stlink_write_reg(sl, reg.r[15] + 2, 15);

                        if (ret) { DLOG("Semihost: write_reg failed for jumping over break\n"); }

                        // continue execution
                        cache_sync(sl);
                        ret = stlink_run(sl, RUN_NORMAL);

                        if (ret) { DLOG("Semihost: continue execution failed with stlink_run\n"); }
                    } else {
                        break;
                    }
                }

                usleep(100000);
            }

            reply = strdup("S05"); // TRAP
            break;

        case 's':
            cache_sync(sl);
            ret = stlink_step(sl);

            if (ret) {
                // ... having a problem sending step packet
                ELOG("Step: cannot send step request\n");
                reply = strdup("E00");
                critical_error = 1; // absolutely critical
            } else {
                reply = strdup("S05"); // TRAP
            }

            break;

        case '?':

            if (attached) {
                reply = strdup("S05"); // TRAP
            } else {
                reply = strdup("OK"); // stub shall reply OK if not attached
            }

            break;

        case 'g':
            ret = stlink_read_all_regs(sl, &regp);

            if (ret) { DLOG("g packet: read_all_regs failed\n"); }

            reply = calloc(8 * 16 + 1, 1);

            for (int32_t i = 0; i < 16; i++) {
                sprintf(&reply[i * 8], "%08x", (uint32_t)htonl(regp.r[i]));
            }

            break;

        case 'p': {
            uint32_t id = (uint32_t)strtoul(&packet[1], NULL, 16);
            uint32_t myreg = 0xDEADDEAD;

            if (id < 16) {
                ret = stlink_read_reg(sl, id, &regp);
                myreg = htonl(regp.r[id]);
            } else if (id == 0x19) {
                ret = stlink_read_reg(sl, 16, &regp);
                myreg = htonl(regp.xpsr);
            } else if (id == 0x1A) {
                ret = stlink_read_reg(sl, 17, &regp);
                myreg = htonl(regp.main_sp);
            } else if (id == 0x1B) {
                ret = stlink_read_reg(sl, 18, &regp);
                myreg = htonl(regp.process_sp);
            } else if (id == 0x1C) {
                ret = stlink_read_unsupported_reg(sl, id, &regp);
                myreg = htonl(regp.control);
            } else if (id == 0x1D) {
                ret = stlink_read_unsupported_reg(sl, id, &regp);
                myreg = htonl(regp.faultmask);
            } else if (id == 0x1E) {
                ret = stlink_read_unsupported_reg(sl, id, &regp);
                myreg = htonl(regp.basepri);
            } else if (id == 0x1F) {
                ret = stlink_read_unsupported_reg(sl, id, &regp);
                myreg = htonl(regp.primask);
            } else if (id >= 0x20 && id < 0x40) {
                ret = stlink_read_unsupported_reg(sl, id, &regp);
                myreg = htonl(regp.s[id - 0x20]);
            } else if (id == 0x40) {
                ret = stlink_read_unsupported_reg(sl, id, &regp);
                myreg = htonl(regp.fpscr);
            } else {
                ret = 1;
                reply = strdup("E00");
            }

            if (ret) { DLOG("p packet: could not read register with id %u\n", id); }

            if (reply == NULL) {
                // if reply is set to "E00", skip
                reply = calloc(8 + 1, 1);
                sprintf(reply, "%08x", myreg);
            }

            break;
        }

        case 'P': {
            char* s_reg = &packet[1];
            char* s_value = strstr(&packet[1], "=") + 1;

            uint32_t reg   = (uint32_t)strtoul(s_reg,   NULL, 16);
            uint32_t value = (uint32_t)strtoul(s_value, NULL, 16);


            if (reg < 16) {
                ret = stlink_write_reg(sl, ntohl(value), reg);
            } else if (reg == 0x19) {
                ret = stlink_write_reg(sl, ntohl(value), 16);
            } else if (reg == 0x1A) {
                ret = stlink_write_reg(sl, ntohl(value), 17);
            } else if (reg == 0x1B) {
                ret = stlink_write_reg(sl, ntohl(value), 18);
            } else if (reg == 0x1C) {
                ret = stlink_write_unsupported_reg(sl, ntohl(value), reg, &regp);
            } else if (reg == 0x1D) {
                ret = stlink_write_unsupported_reg(sl, ntohl(value), reg, &regp);
            } else if (reg == 0x1E) {
                ret = stlink_write_unsupported_reg(sl, ntohl(value), reg, &regp);
            } else if (reg == 0x1F) {
                ret = stlink_write_unsupported_reg(sl, ntohl(value), reg, &regp);
            } else if (reg >= 0x20 && reg < 0x40) {
                ret = stlink_write_unsupported_reg(sl, ntohl(value), reg, &regp);
            } else if (reg == 0x40) {
                ret = stlink_write_unsupported_reg(sl, ntohl(value), reg, &regp);
            } else {
                ret = 1;
                reply = strdup("E00");
            }

            if (ret) { DLOG("P packet: stlink_write_unsupported_reg failed with reg %u\n", reg); }

            if (reply == NULL) { reply = strdup("OK"); /* Note: NULL may not be zero */ }

            break;
        }

        case 'G':

            for (int32_t i = 0; i < 16; i++) {
                char str[9] = {0};
                strncpy(str, &packet[1 + i * 8], 8);
                uint32_t reg = (uint32_t)strtoul(str, NULL, 16);
                ret = stlink_write_reg(sl, ntohl(reg), i);

                if (ret) { DLOG("G packet: stlink_write_reg failed"); }
            }

            reply = strdup("OK");
            break;

        case 'm': {
            char* s_start = &packet[1];
            char* s_count = strstr(&packet[1], ",") + 1;

            stm32_addr_t start = (stm32_addr_t)strtoul(s_start, NULL, 16);
            uint32_t count = (uint32_t)strtoul(s_count, NULL, 16);

            uint32_t adj_start = start % 4;
            uint32_t count_rnd = (count + adj_start + 4 - 1) / 4 * 4;

            if (count_rnd > sl->flash_pgsz) { count_rnd = sl->flash_pgsz; }

            if (count_rnd > 0x1800) { count_rnd = 0x1800; }

            if (count_rnd < count) { count = count_rnd; }

            if (stlink_read_mem32(sl, start - adj_start, count_rnd) != 0) { count = 0; }

            // read failed somehow, don't return stale buffer

            reply = calloc(count * 2 + 1, 1);

            for (uint32_t i = 0; i < count; i++) {
                reply[i * 2 + 0] = hex[sl->q_buf[i + adj_start] >> 4];
                reply[i * 2 + 1] = hex[sl->q_buf[i + adj_start] & 0xf];
            }

            break;
        }

        case 'M': {
            char* s_start = &packet[1];
            char* s_count = strstr(&packet[1], ",") + 1;
            char* hexdata = strstr(packet, ":") + 1;

            stm32_addr_t start = (stm32_addr_t)strtoul(s_start, NULL, 16);
            uint32_t count = (uint32_t)strtoul(s_count, NULL, 16);
            int32_t err = 0;

            if (start % 4) {
                uint32_t align_count = 4 - start % 4;

                if (align_count > count) { align_count = count; }

                for (uint32_t i = 0; i < align_count; i++) {
                    char hextmp[3] = { hexdata[i * 2], hexdata[i * 2 + 1], 0 };
                    uint8_t byte = (uint8_t)strtoul(hextmp, NULL, 16);
                    sl->q_buf[i] = byte;
                }

                err |= stlink_write_mem8(sl, start, align_count);
                cache_change(start, align_count);
                start += align_count;
                count -= align_count;
                hexdata += 2 * align_count;
            }

            if (count - count % 4) {
                uint32_t aligned_count = count - count % 4;

                for (uint32_t i = 0; i < aligned_count; i++) {
                    char hextmp[3] = { hexdata[i * 2], hexdata[i * 2 + 1], 0 };
                    uint8_t byte = (uint8_t)strtoul(hextmp, NULL, 16);
                    sl->q_buf[i] = byte;
                }

                err |= stlink_write_mem32(sl, start, aligned_count);
                cache_change(start, aligned_count);
                count -= aligned_count;
                start += aligned_count;
                hexdata += 2 * aligned_count;
            }

            if (count) {
                for (uint32_t i = 0; i < count; i++) {
                    char hextmp[3] = { hexdata[i * 2], hexdata[i * 2 + 1], 0 };
                    uint8_t byte = (uint8_t)strtoul(hextmp, NULL, 16);
                    sl->q_buf[i] = byte;
                }

                err |= stlink_write_mem8(sl, start, count);
                cache_change(start, count);
            }

            reply = strdup(err ? "E00" : "OK");
            break;
        }

        case 'Z': {
            char *endptr;
            stm32_addr_t addr = (stm32_addr_t)strtoul(&packet[3], &endptr, 16);
            stm32_addr_t len  = (stm32_addr_t)strtoul(&endptr[1], NULL, 16);

            switch (packet[1]) {
            case '1':

                if (update_code_breakpoint(sl, addr, 1) < 0) {
                    reply = strdup("E00");
                } else {
                    reply = strdup("OK");
                }

                break;

            case '2':           // insert write watchpoint
            case '3':           // insert read  watchpoint
            case '4': {         // insert access watchpoint
                enum watchfun wf;

                if (packet[1] == '2') {
                    wf = WATCHWRITE;
                } else if (packet[1] == '3') {
                    wf = WATCHREAD;
                } else {
                    wf = WATCHACCESS;
                }

                if (add_data_watchpoint(sl, wf, addr, len) < 0) {
                    reply = strdup("E00");
                } else {
                    reply = strdup("OK");
                    break;
                }
            }
            break;

            default:
                reply = strdup("");
            }
            break;
        }
        case 'z': {
            char *endptr;
            stm32_addr_t addr = (stm32_addr_t)strtoul(&packet[3], &endptr, 16);
            // stm32_addr_t len  = strtoul(&endptr[1], NULL, 16);

            switch (packet[1]) {
            case '1':          // remove breakpoint
                update_code_breakpoint(sl, addr, 0);
                reply = strdup("OK");
                break;

            case '2':          // remove write watchpoint
            case '3':          // remove read watchpoint
            case '4':          // remove access watchpoint

                if (delete_data_watchpoint(sl, addr) < 0) {
                    reply = strdup("E00");
                    break;
                } else {
                    reply = strdup("OK");
                    break;
                }

            default:
                reply = strdup("");
            }
            break;
        }

        case '!': {
            // enter extended mode which allows restarting. We do support that always.
            // also, set to persistent mode to allow GDB disconnect.
            st->persistent = 1;

            reply = strdup("OK");
            break;
        }

        case 'R': {
            // reset the core.
            ret = stlink_reset(sl, RESET_SOFT_AND_HALT);
            if (ret) { DLOG("R packet : stlink_reset failed\n"); }

            init_code_breakpoints(sl);
            init_data_watchpoints(sl);

            attached = 1;

            reply = strdup("OK");
            break;
        }
        case 'k':
            // kill request - reset the connection itself
            ret = stlink_run(sl, RUN_NORMAL);
            if (ret) { DLOG("Kill: stlink_run failed\n"); }

            ret = stlink_exit_debug_mode(sl);
            if (ret) { DLOG("Kill: stlink_exit_debug_mode failed\n"); }

            stlink_close(sl);

            sl = stlink_open_usb(st->logging_level, st->connect_mode, st->serialnumber, st->freq);
            if (sl == NULL || sl->chip_id == STM32_CHIPID_UNKNOWN) { cleanup(0); }

            connected_stlink = sl;

            ret = stlink_force_debug(sl);
            if (ret) { DLOG("Kill: stlink_force_debug failed\n"); }

            init_cache(sl);
            init_code_breakpoints(sl);
            init_data_watchpoints(sl);

            reply = NULL; // no response
            break;

        default:
            reply = strdup("");
        }

        if (reply) {
            DLOG("send: %s\n", reply);

            int32_t result = gdb_send_packet(client, reply);

            if (result != 0) {
                ELOG("cannot send: %d\n", result);
                free(reply);
                free(packet);
                close_socket(client);
                return (1);
            }

            free(reply);
        }

        if (critical_error) {
            close_socket(client);
            return (1);
        }

        free(packet);
    }

    close_socket(client);
    return (0);
}
