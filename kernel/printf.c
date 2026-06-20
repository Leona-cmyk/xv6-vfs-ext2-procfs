#include <stdarg.h>

#include "types.h"

// 定义va_list类型（如果没有定义）
#ifndef _VA_LIST_DEFINED
#define _VA_LIST_DEFINED
typedef __builtin_va_list va_list;
#endif

#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "memlayout.h"

// 辅助函数：将整数转换为字符串
static int
snprintf_int(char *str, int size, int num)
{
  char buf[16];
  int i = 0, j = 0, neg = 0;

  if(num < 0) {
    neg = 1;
    num = -num;
  }

  if(num == 0) {
    if(size > 1) str[j++] = '0';
  } else {
    while(num > 0 && j < size - 1) {
      buf[i++] = '0' + (num % 10);
      num /= 10;
    }

    if(neg && j < size - 1) str[j++] = '-';

    while(i > 0 && j < size - 1) {
      str[j++] = buf[--i];
    }
  }

  return j;
}

// 辅助函数：将整数转换为十六进制字符串
static int
snprintf_hex(char *str, int size, int num)
{
  char buf[16];
  int i = 0, j = 0;
  const char *hex = "0123456789abcdef";

  if(num == 0) {
    if(size > 1) str[j++] = '0';
  } else {
    while(num > 0 && j < size - 1) {
      buf[i++] = hex[num % 16];
      num /= 16;
    }

    while(i > 0 && j < size - 1) {
      str[j++] = buf[--i];
    }
  }

  return j;
}

// 辅助函数：复制字符串
static int
snprintf_str(char *str, int size, char *s)
{
  int i = 0, j = 0;

  if(!s) s = "(null)";

  while(s[i] && j < size - 1) {
    str[j++] = s[i++];
  }

  return j;
}

static struct {
  struct spinlock lock;
  int locking;
} pr;

int panicked = 0; // Set to 1 to freeze other CPUs
volatile int panicking = 0; // Set to 1 while panicking

static void
printint(int xx, int base, int sign)
{
  static char digits[] = "0123456789abcdef";
  char buf[16];
  int i = 0;
  uint x;

  if(sign && (sign = xx < 0))
    x = -xx;
  else
    x = xx;

  do {
    buf[i++] = digits[x % base];
  } while((x /= base) != 0);

  if(sign)
    buf[i++] = '-';

  while(--i >= 0)
    consputc(buf[i]);
}

// Print to the console. only understands %d, %x, %p, %s.
int
printf(char *fmt, ...)
{
  va_list ap;
  int i, c, locking;
  char *s;

  if(fmt == 0)
    panic("null fmt");

  locking = pr.locking;
  if(locking)
    acquire(&pr.lock);

  va_start(ap, fmt);
  for(i = 0; (c = fmt[i] & 0xff) != 0; i++){
    if(c != '%'){
      consputc(c);
      continue;
    }
    c = fmt[++i] & 0xff;
    if(c == 0)
      break;
    switch(c){
    case 'd':
      printint(va_arg(ap, int), 10, 1);
      break;
    case 'x':
      printint(va_arg(ap, int), 16, 0);
      break;
    case 'p':
      printint(va_arg(ap, uint64), 16, 0);
      break;
    case 's':
      if((s = va_arg(ap, char*)) == 0)
        s = "(null)";
      for(; *s; s++)
        consputc(*s);
      break;
    case '%':
      consputc('%');
      break;
    default:
      // Print unknown % sequence to draw attention.
      consputc('%');
      consputc(c);
      break;
    }
  }

  if(locking)
    release(&pr.lock);

  return 0;
}

void
panic(char *s)
{
  pr.locking = 0;
  panicking = 1; // tell other CPUs to stop
  printf("panic: %s\n", s);
  panicked = 1; // freeze uart output from other CPUs
  for(;;)
    ;
}

void
printfinit(void)
{
  initlock(&pr.lock, "pr");
  pr.locking = 1;
}

// 简单的vsnprintf实现
int
vsnprintf(char *str, int size, const char *fmt, va_list ap)
{
  int i = 0, j = 0;

  while(fmt[i] && j < size - 1) {
    if(fmt[i] != '%') {
      str[j++] = fmt[i++];
    } else {
      i++;
      switch(fmt[i]) {
      case 'd':
        j += snprintf_int(str + j, size - j, va_arg(ap, int));
        i++;
        break;
      case 'x':
        j += snprintf_hex(str + j, size - j, va_arg(ap, int));
        i++;
        break;
      case 's':
        j += snprintf_str(str + j, size - j, va_arg(ap, char*));
        i++;
        break;
      case 'c':
        if(j < size - 1) str[j++] = (char)va_arg(ap, int);
        i++;
        break;
      case '%':
        if(j < size - 1) str[j++] = '%';
        i++;
        break;
      default:
        if(j < size - 1) str[j++] = fmt[i-1];
        if(j < size - 1) str[j++] = fmt[i];
        i++;
        break;
      }
    }
  }

  str[j] = '\0';
  return j;
}

// 简单的snprintf实现
int
snprintf(char *str, int size, const char *format, ...)
{
  va_list ap;
  int len;

  va_start(ap, format);
  len = vsnprintf(str, size, format, ap);
  va_end(ap);

  return len;
}
