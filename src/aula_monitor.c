#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/swap.h>
#include <linux/cpumask.h>
#include <linux/kernel_stat.h>
#include <linux/kprobes.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("PAN");
MODULE_DESCRIPTION("Aula");

#define AULA_VID 0x258a
#define AULA_PID 0x010c
#define PACKET_BUFFER_SIZE 520

// смещения для каждого цветового блока в пакете данных
#define RED_BLOCK_OFFSET   8
#define GREEN_BLOCK_OFFSET 134
#define BLUE_BLOCK_OFFSET  260

enum KeyIndex {
    KEY_PD,         KEY_TILDE,      KEY_TAB,        KEY_CAPSLOCK,   KEY_LSHIFT,     KEY_LCTRL,      KEY_EXCLAMATION,    KEY_1,
    KEY_Q,          KEY_A,          KEY_Z,          KEY_WIN,        KEY_F1,         KEY_2,          KEY_W,              KEY_S,
    KEY_X,          KEY_LALT,       KEY_F2,         KEY_3,          KEY_E,          KEY_D,          KEY_C,              KEY_UNKNOWN_1,
    KEY_F3,         KEY_4,          KEY_R,          KEY_F,          KEY_V,          KEY_UNKNOWN_2,  KEY_F4,             KEY_5,
    KEY_T,          KEY_G,          KEY_B,          KEY_SPACE,      KEY_F5,         KEY_6,          KEY_Y,              KEY_H,
    KEY_N,          KEY_UNKNOWN_3,  KEY_F6,         KEY_7,          KEY_U,          KEY_J,          KEY_M,              KEY_UNKNOWN_4,
    KEY_F7,         KEY_8,          KEY_I,          KEY_K,          KEY_COMMA,      KEY_FN,         KEY_F8,             KEY_9,
    KEY_O,          KEY_L,          KEY_DOT,        KEY_RCTRL,      KEY_F9,         KEY_0,          KEY_P,              KEY_SEMICOLON,
    KEY_QUESTION,   KEY_UNKNOWN_5,  KEY_F10,        KEY_MINUS,      KEY_LBRACKET,   KEY_APOSTROPHE, KEY_RSHIFT,         KEY_UNKNOWN_6,
    KEY_F11,        KEY_EQUALS,     KEY_RBRACKET,   KEY_UNKNOWN_7,  KEY_UNKNOWN_8,  KEY_LARROW,     KEY_F12,            KEY_BACKSPACE,
    KEY_BACKSLASH,  KEY_ENTER,      KEY_UARROW,     KEY_DARROW,     KEY_UNKNOWN_9,  KEY_DEL,        KEY_PGUP,           KEY_ESC,
    KEY_END,        KEY_RARROW,
    KEY_COUNT // 90
};

// индикаторные шкалы
static const enum KeyIndex cpu_keys[] = {
    KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U, KEY_I, KEY_O, KEY_P, KEY_LBRACKET
};
static const enum KeyIndex ram_keys[] = {
    KEY_A, KEY_S, KEY_D, KEY_F, KEY_G, KEY_H, KEY_J, KEY_K, KEY_L, KEY_SEMICOLON
};
static const enum KeyIndex swap_keys[] = {
    KEY_Z, KEY_X, KEY_C, KEY_V, KEY_B, KEY_N, KEY_M, KEY_COMMA, KEY_DOT, KEY_QUESTION
};

static const enum KeyIndex border_keys[] = {
    KEY_TAB, KEY_Q, KEY_CAPSLOCK, KEY_LSHIFT, KEY_RBRACKET, KEY_BACKSLASH, KEY_PGUP, 
    KEY_APOSTROPHE, KEY_ENTER, KEY_ESC, KEY_RSHIFT, KEY_UARROW, KEY_END
};

#define NUM_CPU_KEYS ARRAY_SIZE(cpu_keys)
#define NUM_RAM_KEYS ARRAY_SIZE(ram_keys)
#define NUM_SWAP_KEYS ARRAY_SIZE(swap_keys)
#define NUM_BORDER_KEYS ARRAY_SIZE(border_keys)

struct Color { 
    unsigned char r;
    unsigned char g;
    unsigned char b;
};

struct Gradient {
    struct Color start;
    struct Color end;
};

static const struct Gradient cpu_gradient = { {0, 255, 0}, {255, 0, 0} };
static const struct Gradient ram_gradient = { {0, 255, 0}, {255, 0, 0} };
static const struct Gradient swap_gradient = { {0, 255, 0}, {255, 0, 0} };

typedef void (*si_swapinfo_t)(struct sysinfo *);
static si_swapinfo_t si_swapinfo_func = NULL;

struct aula_kbd {
    struct usb_device *udev;
    struct task_struct *update_thread;
    unsigned char *handshake_packet_1;
    unsigned char *handshake_packet_2;
    unsigned char *color_packet;

    int last_cpu_level;
    int last_ram_level;
    int last_swap_level;

    u64 prev_cpu_total;
    u64 prev_cpu_idle;
};

static void set_key_color(unsigned char *buffer, enum KeyIndex key_idx, struct Color color) {
    if (key_idx < KEY_COUNT) {
        buffer[RED_BLOCK_OFFSET + key_idx] = color.r;
        buffer[GREEN_BLOCK_OFFSET + key_idx] = color.g;
        buffer[BLUE_BLOCK_OFFSET + key_idx] = color.b;
    }
}

static struct Color lint_color(const struct Gradient *grad, int step, int max_steps) {
    struct Color c;
    if (max_steps <= 0) 
        return grad->start;
    if (step < 0) 
        step = 0;
    if (step > max_steps) 
        step = max_steps;

    c.r = grad->start.r + ((grad->end.r - grad->start.r) * step) / max_steps;
    c.g = grad->start.g + ((grad->end.g - grad->start.g) * step) / max_steps;
    c.b = grad->start.b + ((grad->end.b - grad->start.b) * step) / max_steps;

    return c;
}

// Чтение тиков процессора
static void read_cpu_counters(u64 *total, u64 *idle)
{
    int cpu;
    u64 user = 0, nice = 0, system = 0, irq = 0, softirq = 0, steal = 0;
    u64 idle_ticks = 0, iowait = 0;

    *total = 0;
    *idle = 0;

    for_each_online_cpu(cpu) {
        struct kernel_cpustat *kcs = &kcpustat_cpu(cpu);

        user    += kcs->cpustat[CPUTIME_USER];
        nice    += kcs->cpustat[CPUTIME_NICE];
        system  += kcs->cpustat[CPUTIME_SYSTEM];
        idle_ticks += kcs->cpustat[CPUTIME_IDLE];
        iowait  += kcs->cpustat[CPUTIME_IOWAIT];
        irq     += kcs->cpustat[CPUTIME_IRQ];
        softirq += kcs->cpustat[CPUTIME_SOFTIRQ];
        steal   += kcs->cpustat[CPUTIME_STEAL];
    }

    *idle = idle_ticks + iowait;
    *total = user + nice + system + irq + softirq + steal + *idle;
}

// Расчет CPU %
static int get_cpu_usage(struct aula_kbd *dev)
{
    u64 current_total, current_idle;
    u64 diff_total, diff_idle;
    int percent = 0;

    read_cpu_counters(&current_total, &current_idle);

    diff_total = current_total - dev->prev_cpu_total;
    diff_idle  = current_idle - dev->prev_cpu_idle;

    // Сохраняем для следующего раза
    dev->prev_cpu_total = current_total;
    dev->prev_cpu_idle  = current_idle;

    // Если это первый запуск или счетчики переполнились/сбросились
    if (diff_total == 0) return 0;

    if (diff_total > 0) {
        u64 diff_usage = diff_total - diff_idle;
        percent = (int)((diff_usage * 100 + diff_total / 2) / diff_total);
    }

    return percent;
}

// Расчет RAM %
static int get_ram_usage(void)
{
    struct sysinfo si;
    long available_ram;
    unsigned long used_ram;
    int percent = 0;

    si_meminfo(&si);
    available_ram = si_mem_available();

    if (si.totalram > 0) {
        if (available_ram < si.totalram)
            used_ram = si.totalram - available_ram;
        else
            used_ram = 0;

        percent = (int)((used_ram * 100 + si.totalram / 2) / si.totalram);
    }

    return percent;
}

// Расчет SWAP %
static int get_swap_usage(void)
{
    struct sysinfo si;
    int percent = 0;

    si.totalswap = 0;
    si.freeswap = 0;

    if (si_swapinfo_func) {
        si_swapinfo_func(&si);
    }

    if (si.totalswap > 0) {
        unsigned long used_swap = si.totalswap - si.freeswap;
        percent = (int)((used_swap * 100 + si.totalswap / 2) / si.totalswap);
    }

    return percent;
}


static void build_color_packet(unsigned char* buffer, int cpu_to_light, int ram_to_light, int swap_to_light) {
    struct Color c;
    // заголовок пакета
    memcpy(buffer, "\x06\x06\x00\x00\x01\x00\x80\x01", 8);

    for (int i = 0; i < cpu_to_light; i++) {
        c = lint_color(&cpu_gradient, i, NUM_CPU_KEYS - 1);
        set_key_color(buffer, cpu_keys[i], c);
    }

    for (int i = 0; i < ram_to_light; i++) {
        c = lint_color(&ram_gradient, i, NUM_RAM_KEYS - 1);
        set_key_color(buffer, ram_keys[i], c);
    }

    for (int i = 0; i < swap_to_light; i++) {
        c = lint_color(&swap_gradient, i, NUM_SWAP_KEYS - 1);
        set_key_color(buffer, swap_keys[i], c);
    }
    c = (struct Color) {255, 255, 255};
    for (int i = 0; i < NUM_BORDER_KEYS; i++)
        set_key_color(buffer, border_keys[i], c); // Border = Белый
}

static void build_handshake_packet_1(unsigned char* buffer) {
    static const unsigned char data[] = { 
        0x06, 0x84, 0x00, 0x00, 0x01, 0x00, 0x80 
    };
    memcpy(buffer, data, sizeof(data));
}

static void build_handshake_packet_2(unsigned char* buffer) {
    static const unsigned char data[] = {
        0x06, 0x04, 0x00, 0x00, 0x01, 0x00, 0x80, 0x00, 0x00, 0x03, 0x03, 0x01, 0x00, 0x00, 0x04, 0x04,
        0x07, 0x01, 0x15, 0x20, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x01, 0x00, 0xff,
        0x0a, 0x00, 0x01, 0x00, 0x01, 0x00, 0x03, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
        0xff, 0xff, 0x08, 0x40, 0x09, 0x47, 0x09, 0x47, 0x09, 0x47, 0x09, 0x47, 0x09, 0x47, 0x09, 0x47, 
        0x09, 0x47, 0x09, 0x47, 0x09, 0x47, 0x09, 0x47, 0x09, 0x47, 0x09, 0x47, 0x09, 0x47, 0x09, 0x47, 
        0x09, 0x47, 0x03, 0x37, 0x09, 0x37, 0x09, 0x37, 0x09, 0x37, 0x07, 0x47, 0x07, 0x47, 0x07, 0x44, 
        0x07, 0x44, 0x07, 0x44, 0x07, 0x44, 0x07, 0x44, 0x07, 0x44, 0x07, 0x44, 0x04, 0x09, 0x04, 0x04, 
        0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x5a, 0xa5
    };
    memcpy(buffer, data, sizeof(data));
}

static void send_packet(struct usb_device *udev, unsigned char *data) {
    int ret = usb_control_msg(
        udev, 
        usb_sndctrlpipe(udev, 0),         
        0x09, 
        0x21, 
        0x0306, 
        1, 
        data, 
        PACKET_BUFFER_SIZE, 
        5000
    );
    if (ret < 0) 
        printk(KERN_WARNING "AULA Static: Ошибка отправки пакета: %d\n", ret);
    else 
        printk(KERN_INFO "AULA Static: Пакет успешно отправлен\n");
}

static int update_thread_func(void *data) {
    struct aula_kbd *dev = data;
    bool needs_update = false;
    int cpu_percent, ram_percent, swap_percent;

    read_cpu_counters(&dev->prev_cpu_total, &dev->prev_cpu_idle);

    printk(KERN_INFO "AULA: Поток обновления запущен\n");

    while (!kthread_should_stop()) {
        cpu_percent = get_cpu_usage(dev);
        ram_percent = get_ram_usage();
        swap_percent = get_swap_usage();

        // округление вверх
        int current_cpu_level = (cpu_percent == 0) ? 0 : (cpu_percent + 9) / 10;
        int current_ram_level = (ram_percent == 0) ? 0 : (ram_percent + 9) / 10;
        int current_swap_level = (swap_percent == 0) ? 0 : (swap_percent + 9) / 10;

        needs_update = false;
        if (current_cpu_level != dev->last_cpu_level ||
            current_ram_level != dev->last_ram_level ||
            current_swap_level != dev->last_swap_level) {

            needs_update = true;
            dev->last_cpu_level = current_cpu_level;
            dev->last_ram_level = current_ram_level;
            dev->last_swap_level = current_swap_level;
        }

        if (needs_update) {
            send_packet(dev->udev, dev->handshake_packet_1);
            msleep(20);

            send_packet(dev->udev, dev->handshake_packet_2);
            msleep(20);

            memset(dev->color_packet, 0, PACKET_BUFFER_SIZE);
            build_color_packet(dev->color_packet, current_cpu_level, current_ram_level, current_swap_level);
            send_packet(dev->udev, dev->color_packet);
        }
        msleep(3000);
    }
    printk(KERN_INFO "AULA: Поток обновления остановлен\n");
    return 0;
}

static int aula_probe(struct usb_interface *interface, const struct usb_device_id *id) {
    struct aula_kbd *dev;
    struct usb_device *udev = interface_to_usbdev(interface);

    printk(KERN_INFO "AULA: Probe вызван\n");

    dev = kzalloc(sizeof(*dev), GFP_KERNEL);
    if (!dev) 
        return -ENOMEM;
    
    dev->handshake_packet_1 = kzalloc(PACKET_BUFFER_SIZE, GFP_KERNEL);
    dev->handshake_packet_2 = kzalloc(PACKET_BUFFER_SIZE, GFP_KERNEL);
    dev->color_packet = kzalloc(PACKET_BUFFER_SIZE, GFP_KERNEL);

    if (!dev->handshake_packet_1 || !dev->handshake_packet_2 || !dev->color_packet) {
        kfree(dev->handshake_packet_1); 
        kfree(dev->handshake_packet_2); 
        kfree(dev->color_packet);
        kfree(dev);
        return -ENOMEM;
    }
    
    dev->udev = usb_get_dev(udev);
    usb_set_intfdata(interface, dev);

    dev->last_cpu_level = -1;
    dev->last_ram_level = -1;
    dev->last_swap_level = -1;

    build_handshake_packet_1(dev->handshake_packet_1);
    build_handshake_packet_2(dev->handshake_packet_2);
    
    printk(KERN_INFO "AULA: Запуск потока обновления...\n");
    dev->update_thread = kthread_run(update_thread_func, dev, "aula_update_thread");
    if (IS_ERR(dev->update_thread)) {
        printk(KERN_ERR "AULA: Не удалось создать поток ядра\n");
        kfree(dev->handshake_packet_1); 
        kfree(dev->handshake_packet_2); 
        kfree(dev->color_packet);
        kfree(dev);
        return PTR_ERR(dev->update_thread);
    }
    
    return 0;
}

static void aula_disconnect(struct usb_interface *interface) {
    struct aula_kbd *dev = usb_get_intfdata(interface);
    if (dev) {
        if (dev->update_thread) {
            kthread_stop(dev->update_thread);
        }
        
        kfree(dev->handshake_packet_1);
        kfree(dev->handshake_packet_2);
        kfree(dev->color_packet);
        usb_put_dev(dev->udev);
        kfree(dev);
    }
    printk(KERN_INFO "AULA: Клавиатура отключена\n");
}

static const struct usb_device_id aula_table[] = { { USB_DEVICE(AULA_VID, AULA_PID) }, {} };
MODULE_DEVICE_TABLE(usb, aula_table);
static struct usb_driver aula_driver = {
    .name = "aula_monitor",
    .probe = aula_probe,
    .disconnect = aula_disconnect,
    .id_table = aula_table,
};

static int __init aula_init(void) {   
    // достаём функцию si_swapinfo
    struct kprobe kp = {
        .symbol_name = "si_swapinfo",
    };
    int ret;

    ret = register_kprobe(&kp);
    if (ret < 0) {
        printk(KERN_WARNING "AULA Monitor: si_swapinfo не найдена, статистика SWAP будет равна 0.\n");
        si_swapinfo_func = NULL;
    } else {
        si_swapinfo_func = (si_swapinfo_t)kp.addr;
        unregister_kprobe(&kp); // Нам нужен только адрес, сам хук не ставим
        printk(KERN_INFO "AULA Monitor: Функция si_swapinfo найдена по адресу %p\n", si_swapinfo_func);
    }

    return usb_register(&aula_driver); 
}
static void __exit aula_exit(void) { 
    usb_deregister(&aula_driver); 
}

module_init(aula_init);
module_exit(aula_exit);
