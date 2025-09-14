// kernel.c — freestanding tiny kernel C
// Compile with -ffreestanding -fno-pic -fno-pie -nostdlib -m32

#include "stdint.h"

#define PIC1_CMD 0x20
#define PIC1_DATA 0x21
#define PIC2_CMD 0xA0
#define PIC2_DATA 0xA1

// PIT ports and constants
#define PIT_CHANNEL0 0x40
#define PIT_CMD      0x43
#define PIT_BASE_HZ 1193182u

#define IDT_SIZE 256

// Simple VGA text write at 0xB8000
static volatile unsigned short* const VGA = (unsigned short*)0xB8000;
static uint8_t scancode = 0;
static char keyb_char = '\0';
static char keyb_pressed = 0;

static const char scancode_table[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n', 0,
    'a','s','d','f','g','h','j','k','l',';','\'','`', 0,'\\',
    'z','x','c','v','b','n','m',',','.','/', 0,'*', 0,' ', // ...
};


void read_keyb()
{
    keyb_char = '\0';
    keyb_pressed = 0;

    if (scancode & 0x80) {
        keyb_pressed = 0;
        scancode = scancode & 0x7F;
    } else if (!(scancode & 0x80)) {
        keyb_pressed = 1;
    }

    if (scancode > 0 && scancode <= 127) {
        keyb_char = scancode_table[scancode];
    }

    scancode = 0;
}

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ __volatile__ ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ __volatile__ ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void disable_interrupts()
{
    __asm__ __volatile__ ("cli");
}

static inline void enable_interrupts()
{
    __asm__ __volatile__ ("sti");
}

// Remap PIC: master to 0x20-0x27, slave to 0x28-0x2F
void pic_remap()
{
    uint8_t a1 = inb(PIC1_DATA);
    uint8_t a2 = inb(PIC2_DATA);

    outb(PIC1_CMD, 0x11); // ICW1: start init
    outb(PIC2_CMD, 0x11);

    outb(PIC1_DATA, 0x20); // ICW2: master vector offset 0x20
    outb(PIC2_DATA, 0x28); // ICW2: slave vector offset 0x28

    outb(PIC1_DATA, 0x04); // ICW3: master has slave on IRQ2
    outb(PIC2_DATA, 0x02); // ICW3: slave identity

    outb(PIC1_DATA, 0x01); // ICW4: 8086 mode
    outb(PIC2_DATA, 0x01);

    outb(PIC1_DATA, a1);   // restore saved masks
    outb(PIC2_DATA, a2);
}

static inline void pic_send_eoi(uint8_t irq)
{
    if (irq >= 8) outb(PIC2_CMD, 0x20); // send to slave
    outb(PIC1_CMD, 0x20);               // send to master
}

static volatile uint64_t ticks_count = 0;

// Mode 2 (rate generator), read/write latch low then high (lo/hi)
void pit_set_frequency(uint32_t hz)
{
    if (hz == 0) return;
    uint32_t divisor = PIT_BASE_HZ / hz;
    if (divisor == 0) divisor = 1;
    uint8_t lo = divisor & 0xFF;
    uint8_t hi = (divisor >> 8) & 0xFF;

    // Command: channel 0, access lobyte/hibyte, mode 2, binary
    outb(PIT_CMD, 0x34);
    outb(PIT_CHANNEL0, lo);
    outb(PIT_CHANNEL0, hi);
}

// C handler called from IRQ wrapper
void pit_tick_handler_c(void)
{
    ticks_count++;
    // [optional] do scheduling, timeouts, etc.

    // send EOI (PIC)
    // extern void pic_send_eoi(uint8_t irq);
    pic_send_eoi(0);
}

void keyb_handler_c()
{
    scancode = inb(0x60);
    pic_send_eoi(1);
}

typedef void (*irq_handler_t)(void);
static irq_handler_t irq_handlers[16];

void register_irq_handler(int irq, void (*handler)(void))
{
    if (irq < 0 || irq > 15) return;
    irq_handlers[irq] = handler;
}

// Initialize PIT and register handler
void pit_init(uint32_t hz)
{
    disable_interrupts();
    pic_remap();              // remap PIC if not already done
    pit_set_frequency(hz);
    register_irq_handler(0, pit_tick_handler_c); // IRQ0 -> handler
    enable_interrupts();
}

void keyb_init()
{
    disable_interrupts();
    register_irq_handler(1, keyb_handler_c);
    enable_interrupts();
}

struct idt_entry
{
    uint16_t offset_low;
    uint16_t sel;
    uint8_t  zero;
    uint8_t  flags;
    uint16_t offset_high;
} __attribute__((packed));

struct idtr
{
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static struct idt_entry idt[IDT_SIZE];
static struct idtr idtr;

extern void irq0_stub(void); // defined in assembly
extern void irq1_stub(void); // defined in assembly

static void set_idt_entry(int vector, void (*isr)(), uint16_t sel, uint8_t flags)
{
    uint32_t addr = (uint32_t)isr;
    idt[vector].offset_low = addr & 0xFFFF;
    idt[vector].sel = sel;
    idt[vector].zero = 0;
    idt[vector].flags = flags; // Interrupt 0x8E, Trap 0x8F, Task 0x85
    idt[vector].offset_high = (addr >> 16) & 0xFFFF;
}

void idt_init()
{
    for (int i = 0; i < IDT_SIZE; ++i) {
        set_idt_entry(i, 0, 0x08, 0x8E); // default entries (null handler)
    }
    // install IRQ0 at vector 0x20
    set_idt_entry(0x20, irq0_stub, 0x08, 0x8E);
    set_idt_entry(0x21, irq1_stub, 0x08, 0x8E);

    idtr.limit = sizeof(idt) - 1;
    idtr.base = (uint32_t)&idt;
    __asm__ __volatile__("lidtl (%0)" : : "r" (&idtr));
}

// Called from assembly ISR wrapper
void irq_dispatch(int irq)
{
    if (irq_handlers[irq]) irq_handlers[irq]();
}

static short curr_frame[2000];
static short next_frame[2000];


void clear_screen()
{
    // Clear screen (80x25)
    for (int y = 0; y < 25; ++y) {
        for (int x = 0; x < 80; ++x) {
            VGA[y*80 + x] = 0x0700 | ' ';
        }
    }
}

void print_string(char* str, uint16_t colour, int row, int col)
{
    int i = 0;

    while (str[i]) {
        VGA[row*80 + col + i] = colour | (unsigned char)str[i];
        i++;
    }
}

void print_char(char *chr, uint16_t colour, int row, int col)
{
    VGA[row*80 + col] = colour | (unsigned char)*chr;
}

void init_frame_store()
{
    for (int i = 0; i < 2000; ++i) {
        curr_frame[i] = 0x0700;
        next_frame[i] = 0x0700;
    }
}

void draw_next_frame()
{
    for (int i = 0; i < 2000; ++i) {
        if (curr_frame[i] != next_frame[i]) {
            curr_frame[i] = next_frame[i];

            VGA[i] = curr_frame[i];
        }
    }
}

// TODO
int rand()
{
    return ticks_count;
}

#define GRID_SIZE_X 10
#define GRID_SIZE_Y 20
#define NEXT_GRID_SIZE_X 4
#define NEXT_GRID_SIZE_Y 4

#define PIECE_TYPES 7

#define PIECE_LINE 0
#define PIECE_L 1
#define PIECE_REVERSE_L 2
#define PIECE_SQUARE 3
#define PIECE_5 4
#define PIECE_S 5
#define PIECE_T 6

#define MOVE_DOWN 0
#define MOVE_LEFT 1
#define MOVE_RIGHT 2

#define STATE_CREATE_PIECE 0
#define STATE_DESCEND 1
#define STATE_ROW_FLASH 2
#define STATE_ROW_REMOVE 3
#define STATE_GAME_OVER 4
#define STATE_PAUSED 5

#define INITIAL_FALL_DELAY 90
#define DROP_FALL_DELAY 0

int check_tetrominoe_collision(int tetrominoe[4][2], int grid[GRID_SIZE_X][GRID_SIZE_Y])
{
    return tetrominoe[0][0] >= 0 && tetrominoe[0][0] < GRID_SIZE_X
        && tetrominoe[0][1] >= 0 && tetrominoe[0][1] < GRID_SIZE_Y
        && tetrominoe[1][0] >= 0 && tetrominoe[1][0] < GRID_SIZE_X
        && tetrominoe[1][1] >= 0 && tetrominoe[1][1] < GRID_SIZE_Y
        && tetrominoe[2][0] >= 0 && tetrominoe[2][0] < GRID_SIZE_X
        && tetrominoe[2][1] >= 0 && tetrominoe[2][1] < GRID_SIZE_Y
        && tetrominoe[3][0] >= 0 && tetrominoe[3][0] < GRID_SIZE_X
        && tetrominoe[3][1] >= 0 && tetrominoe[3][1] < GRID_SIZE_Y
        && !grid[tetrominoe[0][0]][tetrominoe[0][1]]
        && !grid[tetrominoe[1][0]][tetrominoe[1][1]]
        && !grid[tetrominoe[2][0]][tetrominoe[2][1]]
        && !grid[tetrominoe[3][0]][tetrominoe[3][1]];
}

void setup_tetrominoe(int tetrominoe[4][2], int piece, int offset_x)
{
    switch (piece) {
        case PIECE_LINE:
            tetrominoe[0][0] = offset_x + 0;
            tetrominoe[0][1] = 1;
            tetrominoe[1][0] = offset_x + 1;
            tetrominoe[1][1] = 1;
            tetrominoe[2][0] = offset_x + 2;
            tetrominoe[2][1] = 1;
            tetrominoe[3][0] = offset_x + 3;
            tetrominoe[3][1] = 1;
            break;

        case PIECE_L:
            tetrominoe[0][0] = offset_x + 0;
            tetrominoe[0][1] = 0;
            tetrominoe[1][0] = offset_x + 2;
            tetrominoe[1][1] = 1;
            tetrominoe[2][0] = offset_x + 1;
            tetrominoe[2][1] = 0;
            tetrominoe[3][0] = offset_x + 2;
            tetrominoe[3][1] = 0;
            break;

        case PIECE_REVERSE_L:
            tetrominoe[0][0] = offset_x + 0;
            tetrominoe[0][1] = 0;
            tetrominoe[1][0] = offset_x + 0;
            tetrominoe[1][1] = 1;
            tetrominoe[2][0] = offset_x + 1;
            tetrominoe[2][1] = 0;
            tetrominoe[3][0] = offset_x + 2;
            tetrominoe[3][1] = 0;
            break;

        case PIECE_SQUARE:
            tetrominoe[0][0] = offset_x + 1;
            tetrominoe[0][1] = 0;
            tetrominoe[1][0] = offset_x + 2;
            tetrominoe[1][1] = 0;
            tetrominoe[2][0] = offset_x + 1;
            tetrominoe[2][1] = 1;
            tetrominoe[3][0] = offset_x + 2;
            tetrominoe[3][1] = 1;
            break;

        case PIECE_5:
            tetrominoe[0][0] = offset_x + 1;
            tetrominoe[0][1] = 0;
            tetrominoe[1][0] = offset_x + 2;
            tetrominoe[1][1] = 0;
            tetrominoe[2][0] = offset_x + 0;
            tetrominoe[2][1] = 1;
            tetrominoe[3][0] = offset_x + 1;
            tetrominoe[3][1] = 1;
            break;

        case PIECE_S:
            tetrominoe[0][0] = offset_x + 0;
            tetrominoe[0][1] = 0;
            tetrominoe[1][0] = offset_x + 1;
            tetrominoe[1][1] = 0;
            tetrominoe[2][0] = offset_x + 1;
            tetrominoe[2][1] = 1;
            tetrominoe[3][0] = offset_x + 2;
            tetrominoe[3][1] = 1;
            break;

        case PIECE_T:
            tetrominoe[0][0] = offset_x + 0;
            tetrominoe[0][1] = 0;
            tetrominoe[1][0] = offset_x + 1;
            tetrominoe[1][1] = 0;
            tetrominoe[2][0] = offset_x + 2;
            tetrominoe[2][1] = 0;
            tetrominoe[3][0] = offset_x + 1;
            tetrominoe[3][1] = 1;
            break;
    }
}

int create_tetrominoe(int tetrominoe[4][2], int grid[GRID_SIZE_X][GRID_SIZE_Y], short block_colours[PIECE_TYPES], int piece)
{
    setup_tetrominoe(tetrominoe, piece, 4);

    if (check_tetrominoe_collision(tetrominoe, grid)) {
        grid[tetrominoe[0][0]][tetrominoe[0][1]] = block_colours[piece];
        grid[tetrominoe[1][0]][tetrominoe[1][1]] = block_colours[piece];
        grid[tetrominoe[2][0]][tetrominoe[2][1]] = block_colours[piece];
        grid[tetrominoe[3][0]][tetrominoe[3][1]] = block_colours[piece];

        return 1;
    }

    return 0;
}

void create_next_tetrominoe(int tetrominoe[4][2], int grid[NEXT_GRID_SIZE_X][NEXT_GRID_SIZE_Y], short block_colours[PIECE_TYPES], int piece)
{
    setup_tetrominoe(tetrominoe, piece, 0);

    for (int x = 0; x < NEXT_GRID_SIZE_X; x++) {
        for (int y = 0; y < NEXT_GRID_SIZE_Y; y++) {
            grid[x][y] = 0;
        }
    }

    grid[tetrominoe[0][0]][tetrominoe[0][1]] = block_colours[piece];
    grid[tetrominoe[1][0]][tetrominoe[1][1]] = block_colours[piece];
    grid[tetrominoe[2][0]][tetrominoe[2][1]] = block_colours[piece];
    grid[tetrominoe[3][0]][tetrominoe[3][1]] = block_colours[piece];
}

void copy_tetrominoe(int src_tetrominoe[4][2], int dst_tetrominoe[4][2])
{
    dst_tetrominoe[0][0] = src_tetrominoe[0][0];
    dst_tetrominoe[0][1] = src_tetrominoe[0][1];
    dst_tetrominoe[1][0] = src_tetrominoe[1][0];
    dst_tetrominoe[1][1] = src_tetrominoe[1][1];
    dst_tetrominoe[2][0] = src_tetrominoe[2][0];
    dst_tetrominoe[2][1] = src_tetrominoe[2][1];
    dst_tetrominoe[3][0] = src_tetrominoe[3][0];
    dst_tetrominoe[3][1] = src_tetrominoe[3][1];
}

int move_tetrominoe(int tetrominoe[4][2], int grid[GRID_SIZE_X][GRID_SIZE_Y], int direction)
{
    int moved = 0;
    int new_tetrominoe[4][2];
    short block_colour;

    copy_tetrominoe(tetrominoe, new_tetrominoe);

    switch (direction) {
        case MOVE_DOWN:
            new_tetrominoe[0][1]++;
            new_tetrominoe[1][1]++;
            new_tetrominoe[2][1]++;
            new_tetrominoe[3][1]++;
            break;
        case MOVE_LEFT:
            new_tetrominoe[0][0]--;
            new_tetrominoe[1][0]--;
            new_tetrominoe[2][0]--;
            new_tetrominoe[3][0]--;
            break;
        case MOVE_RIGHT:
            new_tetrominoe[0][0]++;
            new_tetrominoe[1][0]++;
            new_tetrominoe[2][0]++;
            new_tetrominoe[3][0]++;
            break;
    }

    block_colour = grid[tetrominoe[0][0]][tetrominoe[0][1]];

    grid[tetrominoe[0][0]][tetrominoe[0][1]] = 0;
    grid[tetrominoe[1][0]][tetrominoe[1][1]] = 0;
    grid[tetrominoe[2][0]][tetrominoe[2][1]] = 0;
    grid[tetrominoe[3][0]][tetrominoe[3][1]] = 0;

    if (check_tetrominoe_collision(new_tetrominoe, grid)) {
        copy_tetrominoe(new_tetrominoe, tetrominoe);
        moved = 1;
    }

    grid[tetrominoe[0][0]][tetrominoe[0][1]] = block_colour;
    grid[tetrominoe[1][0]][tetrominoe[1][1]] = block_colour;
    grid[tetrominoe[2][0]][tetrominoe[2][1]] = block_colour;
    grid[tetrominoe[3][0]][tetrominoe[3][1]] = block_colour;

    return moved;
}

void rotate_tetrominoe(int tetrominoe[4][2], int grid[GRID_SIZE_X][GRID_SIZE_Y], int type)
{
    int temp_tetrominoe[4][2];
    int new_tetrominoe[4][2];
    int lowest_x;
    int lowest_y;
    int max_ext;

    short block_colour;

    lowest_x = tetrominoe[0][0];
    if (lowest_x > tetrominoe[1][0]) lowest_x = tetrominoe[1][0];
    if (lowest_x > tetrominoe[2][0]) lowest_x = tetrominoe[2][0];
    if (lowest_x > tetrominoe[3][0]) lowest_x = tetrominoe[3][0];

    lowest_y = tetrominoe[0][1];
    if (lowest_y > tetrominoe[1][1]) lowest_y = tetrominoe[1][1];
    if (lowest_y > tetrominoe[2][1]) lowest_y = tetrominoe[2][1];
    if (lowest_y > tetrominoe[3][1]) lowest_y = tetrominoe[3][1];

    temp_tetrominoe[0][0] = tetrominoe[0][0] - lowest_x;
    temp_tetrominoe[1][0] = tetrominoe[1][0] - lowest_x;
    temp_tetrominoe[2][0] = tetrominoe[2][0] - lowest_x;
    temp_tetrominoe[3][0] = tetrominoe[3][0] - lowest_x;
    temp_tetrominoe[0][1] = tetrominoe[0][1] - lowest_y;
    temp_tetrominoe[1][1] = tetrominoe[1][1] - lowest_y;
    temp_tetrominoe[2][1] = tetrominoe[2][1] - lowest_y;
    temp_tetrominoe[3][1] = tetrominoe[3][1] - lowest_y;

    switch (type) {
        case PIECE_LINE:
            max_ext = 4;
            break;

        case PIECE_L:
            max_ext = 3;
            break;

        case PIECE_REVERSE_L:
            max_ext = 3;
            break;

        case PIECE_SQUARE:
            max_ext = 2;
            break;

        case PIECE_5:
            max_ext = 3;
            break;

        case PIECE_S:
            max_ext = 3;
            break;

        case PIECE_T:
            max_ext = 3;
            break;
    }

    new_tetrominoe[0][0] = temp_tetrominoe[0][1];
    new_tetrominoe[0][1] = 1-(temp_tetrominoe[0][0]-(max_ext-2));
    new_tetrominoe[1][0] = temp_tetrominoe[1][1];
    new_tetrominoe[1][1] = 1-(temp_tetrominoe[1][0]-(max_ext-2));
    new_tetrominoe[2][0] = temp_tetrominoe[2][1];
    new_tetrominoe[2][1] = 1-(temp_tetrominoe[2][0]-(max_ext-2));
    new_tetrominoe[3][0] = temp_tetrominoe[3][1];
    new_tetrominoe[3][1] = 1-(temp_tetrominoe[3][0]-(max_ext-2));

    new_tetrominoe[0][0] = new_tetrominoe[0][0] + lowest_x;
    new_tetrominoe[1][0] = new_tetrominoe[1][0] + lowest_x;
    new_tetrominoe[2][0] = new_tetrominoe[2][0] + lowest_x;
    new_tetrominoe[3][0] = new_tetrominoe[3][0] + lowest_x;
    new_tetrominoe[0][1] = new_tetrominoe[0][1] + lowest_y;
    new_tetrominoe[1][1] = new_tetrominoe[1][1] + lowest_y;
    new_tetrominoe[2][1] = new_tetrominoe[2][1] + lowest_y;
    new_tetrominoe[3][1] = new_tetrominoe[3][1] + lowest_y;

    block_colour = grid[tetrominoe[0][0]][tetrominoe[0][1]];

    grid[tetrominoe[0][0]][tetrominoe[0][1]] = 0;
    grid[tetrominoe[1][0]][tetrominoe[1][1]] = 0;
    grid[tetrominoe[2][0]][tetrominoe[2][1]] = 0;
    grid[tetrominoe[3][0]][tetrominoe[3][1]] = 0;

    if (check_tetrominoe_collision(new_tetrominoe, grid)) {
        copy_tetrominoe(new_tetrominoe, tetrominoe);
    }

    grid[tetrominoe[0][0]][tetrominoe[0][1]] = block_colour;
    grid[tetrominoe[1][0]][tetrominoe[1][1]] = block_colour;
    grid[tetrominoe[2][0]][tetrominoe[2][1]] = block_colour;
    grid[tetrominoe[3][0]][tetrominoe[3][1]] = block_colour;
}

int get_remove_lines(int grid[GRID_SIZE_X][GRID_SIZE_Y], int remove_lines[4])
{
    int remove_count = 0;

    for (int y = 0; y < GRID_SIZE_Y; y++) {
        for (int x = 0; x < GRID_SIZE_X; x++) {
            if (!grid[x][y]) {
                goto next_line;
            }
        }

        remove_lines[remove_count++] = y;

        next_line:
        continue;
    }

    return remove_count;
}

void cycle_remove_lines(int grid[GRID_SIZE_X][GRID_SIZE_Y], int remove_lines[4])
{
    for (int i = 0; i < 4; i++) {
        if (remove_lines[i] == -1) { continue; }

        // Cycle the line
        for (int x = 0; x < GRID_SIZE_X; x++) {
            if ((grid[x][remove_lines[i]] & 0x00FF) == '#') {
                grid[x][remove_lines[i]] = grid[x][remove_lines[i]] & 0xFF20;
            } else {
                grid[x][remove_lines[i]] = grid[x][remove_lines[i]] | '#';
            }
        }
    }
}

int do_remove_lines(int grid[GRID_SIZE_X][GRID_SIZE_Y], int remove_lines[4])
{
    int remove_count = 0;

    for (int i = 0; i < 4; i++) {
        if (remove_lines[i] == -1) { continue; }

        remove_count++;

        // Remove the line
        for (int x = 0; x < GRID_SIZE_X; x++) {
            grid[x][remove_lines[i]] = 0;
        }

        // Move above lines down
        for (int y = remove_lines[i]; y != 0; y--) {
            for (int x = 0; x < GRID_SIZE_X; x++) {
                grid[x][y] = grid[x][y-1];
            }
        }

        remove_lines[i] = -1;
    }

    return remove_count;
}

void set_numbers_display(int pos_x, int pos_y, char numbers[10], int value)
{
    int value_tmp = value;
    int digit_count = 0;
    int digit_pos = 0;
    int digits[8];

    for (int i = 0; i < 8; i++) {
        next_frame[(pos_y*80)+pos_x+i] = 0x0F00 | ' ';
    }

    if (value_tmp == 0) {
        digits[digit_count++] = 0;
    } else {
        while (value_tmp) {
            digits[digit_count++] = value_tmp % 10;
            value_tmp /= 10;
        }
    }

    for (int i = digit_count; i > 0; i--) {
        next_frame[(pos_y*80)+pos_x+digit_pos] = 0x0F00 | numbers[digits[i-1]];
        digit_pos++;
    }

}

void tetris()
{
    clear_screen();

    int quit = 0;
    int pause = 0;
    int left = 0;
    int right = 0;
    int up = 0;
    int down = 0;
    int key_pressed = 0;
    int down_pressed = 0;

    int state = STATE_DESCEND;
    int flash_lines_count = 0;
    int lines = 0;
    int level = 0;
    int score = 0;
    int lines_removed;
    int fall_delay = INITIAL_FALL_DELAY;

    int grid[GRID_SIZE_X][GRID_SIZE_Y];
    int next_grid[NEXT_GRID_SIZE_X][NEXT_GRID_SIZE_Y];

    int tetrominoe[4][2];
    int next_tetrominoe[4][2];
    int current;
    int next = -1;
    int remove_lines[4] = {-1, -1, -1, -1};

    uint64_t last_move;
    uint64_t now;
    uint64_t timediff;

    short block_colours[PIECE_TYPES];
    char numbers[10] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9'};

    // Light gray
    block_colours[0] = 0x0700;

    // Red
    block_colours[1] = 0x0400;

    // Green
    block_colours[2] = 0x0200;

    // Blue
    block_colours[3] = 0x0100;

    // Magenta
    block_colours[4] = 0x0500;

    // Yellow
    block_colours[5] = 0x0E00;

    // Cyan
    block_colours[6] = 0x0300;

    for (int i = 0; i < GRID_SIZE_Y; i++) {
        next_frame[(i*80)+GRID_SIZE_X+1] = 0x0F00 | '#';
    }

    for (int i = 0; i < GRID_SIZE_Y; i++) {
        next_frame[(i*80)] = 0x0F00 | '#';
    }

    for (int i = 0; i < GRID_SIZE_X+2; i++) {
        next_frame[(GRID_SIZE_Y*80)+i] = 0x0F00 | '#';
    }

    next_frame[(2*80)+GRID_SIZE_X+6] = 0x0F00 | 'N';
    next_frame[(2*80)+GRID_SIZE_X+7] = 0x0F00 | 'E';
    next_frame[(2*80)+GRID_SIZE_X+8] = 0x0F00 | 'X';
    next_frame[(2*80)+GRID_SIZE_X+9] = 0x0F00 | 'T';
    next_frame[(2*80)+GRID_SIZE_X+10] = 0x0F00 | ':';

    next_frame[(7*80)+GRID_SIZE_X+6] = 0x0F00 | 'L';
    next_frame[(7*80)+GRID_SIZE_X+7] = 0x0F00 | 'I';
    next_frame[(7*80)+GRID_SIZE_X+8] = 0x0F00 | 'N';
    next_frame[(7*80)+GRID_SIZE_X+9] = 0x0F00 | 'E';
    next_frame[(7*80)+GRID_SIZE_X+10] = 0x0F00 | 'S';
    next_frame[(7*80)+GRID_SIZE_X+11] = 0x0F00 | ':';

    next_frame[(8*80)+GRID_SIZE_X+6] = 0x0F00 | 'L';
    next_frame[(8*80)+GRID_SIZE_X+7] = 0x0F00 | 'E';
    next_frame[(8*80)+GRID_SIZE_X+8] = 0x0F00 | 'V';
    next_frame[(8*80)+GRID_SIZE_X+9] = 0x0F00 | 'E';
    next_frame[(8*80)+GRID_SIZE_X+10] = 0x0F00 | 'L';
    next_frame[(8*80)+GRID_SIZE_X+11] = 0x0F00 | ':';

    next_frame[(9*80)+GRID_SIZE_X+6] = 0x0F00 | 'S';
    next_frame[(9*80)+GRID_SIZE_X+7] = 0x0F00 | 'C';
    next_frame[(9*80)+GRID_SIZE_X+8] = 0x0F00 | 'O';
    next_frame[(9*80)+GRID_SIZE_X+9] = 0x0F00 | 'R';
    next_frame[(9*80)+GRID_SIZE_X+10] = 0x0F00 | 'E';
    next_frame[(9*80)+GRID_SIZE_X+11] = 0x0F00 | ':';

    set_numbers_display(GRID_SIZE_X+13, 7, numbers, lines);
    set_numbers_display(GRID_SIZE_X+13, 9, numbers, score);
    set_numbers_display(GRID_SIZE_X+13, 8, numbers, level);

    // Clear the grid
    for (int x = 0; x < GRID_SIZE_X; x++) {
        for (int y = 0; y < GRID_SIZE_Y; y++) {
            grid[x][y] = 0;
        }
    }

    // Clear the next grid
    for (int x = 0; x < NEXT_GRID_SIZE_X; x++) {
        for (int y = 0; y < NEXT_GRID_SIZE_Y; y++) {
            next_grid[x][y] = 0;
        }
    }

    print_string("CONTROLS", 0x0F00, 15, GRID_SIZE_X+6);
    print_string("a - Left", 0x0F00, 16, GRID_SIZE_X+6);
    print_string("d - Right", 0x0F00, 17, GRID_SIZE_X+6);
    print_string("s - Drop", 0x0F00, 18, GRID_SIZE_X+6);
    print_string("w - Rotate", 0x0F00, 19, GRID_SIZE_X+6);
    print_string("p - Pause", 0x0F00, 20, GRID_SIZE_X+6);
    print_string("r - Restart", 0x0F00, 21, GRID_SIZE_X+6);
    print_string("q - Halt CPU", 0x0F00, 22, GRID_SIZE_X+6);

    current = rand() % 7;

    create_tetrominoe(tetrominoe, grid, block_colours, current);

    // Main loop
    while (quit == 0) {
        read_keyb();

        switch (keyb_char) {
            case 'a':
                left = keyb_pressed ? 1 : 0;
                break;
            case 'd':
                right = keyb_pressed ? 1 : 0;
                break;
            case 'w':
                up = keyb_pressed ? 1 : 0;
                break;
            case 's':
                down = keyb_pressed ? 1 : 0;
                break;
            case 'q':
                quit = keyb_pressed ? 1 : 0;
                break;
            case 'p':
                pause = keyb_pressed ? 1 : 0;
                break;
            case 'r':
                if (keyb_pressed) {
                    return;
                }
        }

        if (next == -1) {
            next = rand() % 7;
            create_next_tetrominoe(next_tetrominoe, next_grid, block_colours, next);
        }

        if (!left && !right && !up && !down && !pause) {
            key_pressed = 0;
        }

        if (!down) {
            down_pressed = 0;
        }

        if (state == STATE_DESCEND && !key_pressed && (left || right || up || down || pause)) {
            if (left) { move_tetrominoe(tetrominoe, grid, MOVE_LEFT); }
            if (right) { move_tetrominoe(tetrominoe, grid, MOVE_RIGHT); }
            if (down) { down_pressed = 1; }
            if (up) { rotate_tetrominoe(tetrominoe, grid, current); }
            if (pause) { state = STATE_PAUSED; }

            key_pressed = 1;
        } else if (state == STATE_PAUSED && !key_pressed && pause) {
            print_string("      ", 0x0200, 11, GRID_SIZE_X+6);
            state = STATE_DESCEND;

            key_pressed = 1;
        }

        now = ticks_count;
        timediff = now - last_move;

        switch (state) {
            case STATE_CREATE_PIECE:
                if (create_tetrominoe(tetrominoe, grid, block_colours, next)) {
                    current = next;
                    next = -1;
                    state = STATE_DESCEND;
                    down_pressed = 0;
                } else {
                    state = STATE_GAME_OVER;
                }

                break;

            case STATE_DESCEND:
                if (timediff > (down_pressed ? DROP_FALL_DELAY : fall_delay)) {
                    if (!move_tetrominoe(tetrominoe, grid, MOVE_DOWN)) {
                        if (get_remove_lines(grid, remove_lines) > 0) {
                            state = STATE_ROW_FLASH;
                        } else {
                            state = STATE_CREATE_PIECE;
                        }
                    }

                    last_move = now;
                }

                break;

            case STATE_ROW_FLASH:
                if (timediff > 10) {
                    if (flash_lines_count < 4) {
                        cycle_remove_lines(grid, remove_lines);
                        flash_lines_count++;
                    } else {
                        flash_lines_count = 0;
                        state = STATE_ROW_REMOVE;
                    }

                    last_move = now;
                }

                break;

            case STATE_ROW_REMOVE:
                lines_removed = do_remove_lines(grid, remove_lines);
                lines += lines_removed;
                if (lines > 9999) { lines = 9999; }
                set_numbers_display(GRID_SIZE_X+13, 7, numbers, lines);

                switch (lines_removed) {
                    case 1:
                        score += 40 * (level + 1);
                        break;

                    case 2:
                        score += 100 * (level + 1);
                        break;

                    case 3:
                        score += 300 * (level + 1);
                        break;

                    case 4:
                        score += 1200 * (level + 1);
                        break;
                }

                if (score > 99999999) { score = 99999999; }

                set_numbers_display(GRID_SIZE_X+13, 9, numbers, score);

                if (level != 9 && lines >= (level * 10) + 10) {
                    level++;
                    fall_delay -= 10;

                    set_numbers_display(GRID_SIZE_X+13, 8, numbers, level);
                }

                state = STATE_CREATE_PIECE;

                break;

            case STATE_GAME_OVER:
                print_string("GAME OVER", 0x0400, 12, GRID_SIZE_X+6);

                break;

            case STATE_PAUSED:
                print_string("PAUSED", 0x0200, 11, GRID_SIZE_X+6);

                break;
        }

        // Redraw screen
        for (int x = 0; x < GRID_SIZE_X; x++) {
            for (int y = 0; y < GRID_SIZE_Y; y++) {
                if (grid[x][y] == 0) {
                    next_frame[y*80+x+1] = 0x0700 | ' ';
                } else if ((grid[x][y] & 0x00FF) == ' ') {
                    next_frame[y*80+x+1] = grid[x][y] | ' ';
                } else {
                    next_frame[y*80+x+1] = grid[x][y] | '#';
                }
            }
        }

        for (int x = 0; x < NEXT_GRID_SIZE_X; x++) {
            for (int y = 0; y < NEXT_GRID_SIZE_Y; y++) {
                if (next_grid[x][y] == 0) {
                    next_frame[(y+3)*80+x+GRID_SIZE_X+6] = 0x0700 | ' ';
                } else {
                    next_frame[(y+3)*80+x+GRID_SIZE_X+6] = next_grid[x][y] | '#';
                }
            }
        }

        draw_next_frame();

        __asm__ __volatile__("hlt");
    }

    print_string("CPU HALTED", 0x0100, 13, GRID_SIZE_X+6);

    disable_interrupts();

    for (;;) {
        __asm__ __volatile__("hlt");
    }
}

void main()
{
    idt_init();
    pit_init(100); // 100 Hz tick (10 ms per tick)
    keyb_init();

    for (;;) {
        init_frame_store();
        tetris();
    }
}