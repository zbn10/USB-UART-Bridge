#include <SerialPIO.h>
#include <Adafruit_TinyUSB.h>

#if CFG_TUD_CDC < 7
#error Must change CFG_TUD_CDC >= 7 and Adafruit_USBD_Device._desc_cfg_buffer >= 1024
#endif
/*
tusb_config_rt2040.h
#define CFG_TUD_CDC 7 <-
&
Adafruit_USBD_Device.h
class Adafruit_USBD_Device {
  ...
  uint8_t _desc_cfg_buffer[1024]; <- 256 is not enough
*/

#define UUB_MAX 6
#define READBUFSIZE 64
#define CMDLINE_CHAR_MAX 100
#define CMDLINE_ARGC_MAX 5

class Ascii {
  private:
    static char asciistr[255][5];
    static int initialized;
  public:
    static void Init(void);
    static char *Str(unsigned char);
    static unsigned char Chr(char *);
};

char Ascii::asciistr[255][5] = {
    "\\0",  "\\SOH",  "\\STX",  "\\ETX",
    "\\EOT",  "\\ENQ",  "\\ACK",  "\\a",
    "\\b",    "\\t",    "\\n",    "\\v",
    "\\f",    "\\r",    "\\SO",   "\\SI",
    "\\DLE",  "\\DC1",  "\\DC2",  "\\DC3",
    "\\DC4",  "\\NAK",  "\\SYN",  "\\ETB",
    "\\CAN",  "\\EM",   "\\SUB",  "\\ESC",
    "\\FS",   "\\GS",   "\\RS",   "\\US"
};
int Ascii::initialized = 0;

void Ascii::Init(void) {
  for(int i = 0x20; i <0x100; i++) {
    asciistr[i][0] = i;
    asciistr[i][1] = '\0';
  }
  strcpy(&asciistr[0x7f][0], "\\DEL");

  initialized = 1;
}

char *Ascii::Str(unsigned char code) {
  if(!initialized || code < 0 || code > 255) return(NULL);
  return(asciistr[code]);
}

unsigned char Ascii::Chr(char *str) {
  unsigned char d;

  if(str[0] == '\\' && str[1] != '\0') {
    switch(str[1]) {
      case '0':
        d = '\0';
        break;
      case 'b':
        d = '\b';
        break;
      case 't':
        d = '\t';
        break;
      case 'n':
        d = '\n';
        break;
      case 'v':
        d = '\v';
        break;
      case 'f':
        d = '\f';
        break;
      case 'r':
        d = '\r';
        break;
    }
  } else {
    d = (unsigned char)str[0];
  }

  return(d);
}

class UsbUartBridge {
  private:
    char *descr;
    int baud;
    unsigned int usb2uart_cnt;
    unsigned int uart2usb_cnt;
    SerialUART *uarts;
    SerialPIO *uartp;
    Adafruit_USBD_CDC *usbi;
    Adafruit_USBD_CDC *cap2cons;
#define CAPMODE_TXT 1
#define CAPMODE_HEX 2
    int capmode;
#define CAPBUFSIZE 128
    unsigned char capbuf_usb2uart[CAPBUFSIZE];
    unsigned char capbuf_uart2usb[CAPBUFSIZE];
    unsigned int capbuf_usb2uart_p;
    unsigned int capbuf_uart2usb_p;
    unsigned long capbuf_usb2uart_last;
    unsigned long capbuf_uart2usb_last;
    unsigned char cap_delim;
    void _init(char *, Adafruit_USBD_CDC *, int);
    void _capture(unsigned char *);
    void _capture(int, unsigned char *, unsigned int *, unsigned long *, const char *);
    void _capture_flush(unsigned char *, unsigned int *, unsigned long *, const char *);

  public:
    void init(char *, Adafruit_USBD_CDC *, SerialUART *, int);
    void init(char *, Adafruit_USBD_CDC *, SerialPIO *, int);
    void transmit(void);
    void setCapture(Adafruit_USBD_CDC *);
    Adafruit_USBD_CDC *getCapture(void);
    unsigned int getUsb2Uart(void);
    unsigned int getUart2Usb(void);
    char *getDescr(void);
    int getBaud(void);
    void clearCnt(void);
    int setCapMode(int);
    int getCapMode(void);
    void initCapture(void);
    void setCapDelim(unsigned char);
    unsigned char getCapDelim();
};

void UsbUartBridge::setCapDelim(unsigned char c) {
  cap_delim = c;
}

unsigned char UsbUartBridge::getCapDelim() {
  return(cap_delim);
}

int UsbUartBridge::setCapMode(int mode) {
  capmode = 0;
  if(mode & CAPMODE_TXT) capmode |= CAPMODE_TXT;
  if(mode & CAPMODE_HEX) capmode |= CAPMODE_HEX;
  if(capmode == 0) capmode = CAPMODE_HEX;
  return(capmode);
}

void UsbUartBridge::initCapture(void) {
  capbuf_usb2uart_p = 0;
  capbuf_uart2usb_p = 0;
}

int UsbUartBridge::getCapMode(void) {
  return(capmode);
}

void UsbUartBridge::clearCnt(void) {
  usb2uart_cnt = 0;
  uart2usb_cnt = 0;
}

int UsbUartBridge::getBaud(void)  {
  return(baud);
}

char *UsbUartBridge::getDescr(void) {
  return(descr);
}

void UsbUartBridge::_init(char *descr, Adafruit_USBD_CDC *usbi, int baud) {
  this->descr = descr;
  this->baud = baud;
  uarts = NULL;
  uartp = NULL;
  this->usbi = usbi;

  clearCnt();

  cap_delim = '\n';
  cap2cons = NULL;
  capmode = CAPMODE_HEX;
  initCapture();
}

void UsbUartBridge::init(char *descr, Adafruit_USBD_CDC *usbi, SerialPIO *uart, int baud) {
  _init(descr, usbi, baud);
  uartp = uart;

  uartp->begin(baud);
  usbi->begin(115200);
}

void UsbUartBridge::init(char *descr, Adafruit_USBD_CDC *usbi, SerialUART *uart, int baud) {
  _init(descr, usbi, baud);
  uarts = uart;

  uarts->begin(baud);
  usbi->begin(115200);
}

void UsbUartBridge::_capture(int c, unsigned char *buf, unsigned int *buf_pp,
  unsigned long *tm_p, const char *dir) {

  int pushout = 0;
  buf[(*buf_pp)++] = c;

  if(*buf_pp == 1) {
    *tm_p  = millis();
  }

  if((capmode & CAPMODE_TXT) && (buf[(*buf_pp)-1] == cap_delim || *buf_pp == CAPBUFSIZE)) {
    cap2cons->printf("%s (%s txt):", descr, dir);
    for(int i = 0; i < (*buf_pp); i++) {
      cap2cons->printf("%s", Ascii::Str(buf[i]));
    }
    cap2cons->printf("\n");

    pushout = 1;
  }

  if((capmode & CAPMODE_HEX) &&
    (pushout || (!(capmode & CAPMODE_TXT) && (*buf_pp) == MIN(16, CAPBUFSIZE)))) {

    cap2cons->printf("%s (%s hex):", descr, dir);
    for(int i = 0; i < (*buf_pp); i++) {
      cap2cons->printf(" %02x", buf[i]);
    }
    cap2cons->printf("\n");

    pushout = 1;
  }

  if(pushout) (*buf_pp) = 0;
}

void UsbUartBridge::_capture_flush(unsigned char *buf, unsigned int *buf_pp,
  unsigned long *tm_p, const char *dir) {

  if(millis() - (*tm_p) > 1000) { // correct even if millis() overflow
    if(capmode & CAPMODE_TXT) {
      cap2cons->printf("%s (%s txt):", descr, dir);
      for(int i = 0; i < (*buf_pp); i++) {
        cap2cons->printf("%s", Ascii::Str(buf[i]));
      }
      cap2cons->printf("\n");
    }

    if(capmode & CAPMODE_HEX) {
      cap2cons->printf("%s (%s hex):", descr, dir);
      for(int i = 0; i < (*buf_pp); i++) {
        cap2cons->printf("%02x ", buf[i]);
      }
      cap2cons->printf("\n");
    }

    (*buf_pp) = 0;
  }
}

#define UUB_UART_AVAILABLE(o) (o->uarts ? o->uarts->available() : o->uartp->available())
#define UUB_UART_READ(o) (o->uarts ? o->uarts->read() : o->uartp->read())
#define UUB_UART_WRITE(o, c) (o->uarts ? o->uarts->write(c) : o->uartp->write(c))
#define UUB_UART_FLUSH(o) (o->uarts ? o->uarts->flush() : o->uartp->flush())
void UsbUartBridge::transmit(void) {
  int len, c;
  char buf[READBUFSIZE + 1];
  unsigned long curtime;

  if((len = UUB_UART_AVAILABLE(this)) > 0) {
    while(len--) {
      c = UUB_UART_READ(this);
      if(c >= 0) {
        /*
          temporary hack for SerialPIO strange vehabior
          SerialPIO.available() in package 2.5.2 seems to have something bug.
          it returns 64 regularly evenif empty.
          I think it's a buffer boundary issue.
        */
        usbi->write(c);
        usbi->flush();
        uart2usb_cnt++;

        if(cap2cons) {
          _capture(c, capbuf_uart2usb, &capbuf_uart2usb_p, &capbuf_uart2usb_last, "uart->usb");
        }
      }
    }
  }
  if(cap2cons && capbuf_uart2usb_p) {
    _capture_flush(capbuf_uart2usb, &capbuf_uart2usb_p, &capbuf_uart2usb_last, "uart->usb");
  }


  if((len = usbi->available()) > 0) {
    while(len--) {
      c = usbi->read();
      if(c >= 0) {
        UUB_UART_WRITE(this, c);
        UUB_UART_FLUSH(this);
        usb2uart_cnt++;

        if(cap2cons) {
          _capture(c, capbuf_usb2uart, &capbuf_usb2uart_p, &capbuf_usb2uart_last, "usb->uart");
        }
      }
    }
  }
  if(cap2cons && capbuf_usb2uart_p) {
    _capture_flush(capbuf_usb2uart, &capbuf_usb2uart_p, &capbuf_usb2uart_last, "usb->uart");
  }
}

void UsbUartBridge::setCapture(Adafruit_USBD_CDC *serial) {
  cap2cons = serial;
}

Adafruit_USBD_CDC *UsbUartBridge::getCapture(void) {
  return(cap2cons);
}

unsigned int UsbUartBridge::getUsb2Uart(void) {
  return(usb2uart_cnt);
}

unsigned int UsbUartBridge::getUart2Usb(void) {
  return(uart2usb_cnt);
}


class CliParser {
  private:
    char *cmd;
    int cmdlinemax;
    int cmdlen;
    char **argv;
    int argcmax;
    int argc;

  public:
    CliParser(int, int);
    void initCmdStr(void);
    int getCmdStr(Adafruit_USBD_CDC *);
    int parseCmdStr(void);
    int getArgc(void);
    int getArgv(int);
    int getArgv(int, char*, int);
    int argvCmp(int, const char *);
    void debugPrint(Adafruit_USBD_CDC *);
};

void CliParser::debugPrint(Adafruit_USBD_CDC *serial) {
  int plen = sizeof(char *) * argcmax + cmdlinemax + 1;
  char *p = (char *)argv;

  serial->printf("argcmax %d, sizeof(char *) %d, cmdlinemax %d, malloc size %d\n",
    argcmax, sizeof(char *), cmdlinemax, plen);
  serial->printf("argv = 0, &cmd[0] = %d\n", (int)(cmd-(char *)argv));
  int pp = 0;
  while(pp < argc) {
    serial->printf("argv[%d](%d)->%d, ",
      pp, (int)((char *)&argv[pp]-(char *)argv), (int)(argv[pp]-(char *)argv));
    pp++;
  }
  serial->println();

  serial->printf("argc: %d", argc);
  int i=0;
  while(i < argc) {
    Serial.printf(" [%s]", argv[i]);
    i++;
  }
  Serial.println();
}

CliParser::CliParser(int argcmax, int cmdlinemax) {
  cmd = NULL;
  argv = (char **)NULL;
  this->cmdlinemax = 0;
  this->argcmax = 0;
  initCmdStr();

  if(argcmax < 1 || cmdlinemax < 1) {
    return;
  }

  argv = (char **)malloc(sizeof(char *) * argcmax + cmdlinemax + 1);
  if(!argv) return;

  cmd = ((char *)argv + sizeof(char *) * argcmax);
  this->argcmax = argcmax;
  this->cmdlinemax = cmdlinemax;
}

int CliParser::argvCmp(int n, const char *str) {
  if(n < 0 || n > argc) return(-1);
  return(strcmp(argv[n], str));
}

void CliParser::initCmdStr(void) {
  argc = 0;
  cmdlen = 0;
}

int CliParser::getCmdStr(Adafruit_USBD_CDC *serial) {
  int len;
  int c;
  if((len = serial->available()) == 0) {
    return(0);
  }

  while(len) {
    c = serial->read();
    len--;

    if(c == '\a') continue;
    if(c == '\n') {
      cmd[cmdlen] = '\0';
      return(1);
    }
    if(cmdlen >= cmdlinemax) {
      cmd[cmdlinemax] = '\0';
      continue;
    }
    cmd[cmdlen] = c;
    cmdlen++;
  }

  return(0);
}

int CliParser::parseCmdStr(void) {
  int i = 0;
  int first_char = 1;
  argc = 0;

  while(i < cmdlen) {
    if(cmd[i] == ' ') {
      cmd[i] = '\0';
      i++;
      first_char = 1;
      continue;
    }

    if(first_char) {
      first_char = 0;
      if(argc < argcmax) {
        argv[argc] = cmd+i;
        argc++;
      }
    }

    i++;
  }

  return(argc);
}

int CliParser::getArgc(void) {
  return(argc);
}

int CliParser::getArgv(int n) {
  if(n < 0 || n > argc) return(0);
  return(atoi(argv[n]));
}

int CliParser::getArgv(int n, char *buf, int buflen) {
  // return <0 = err, 0 = lack of buflen, >0 = success
  int len;
  if(n < 0 || n > argc || buf == NULL || buflen < 0) return(-1);

  len = strlen(argv[n]);
  if(buflen >= len + 1) {
    memcpy(buf, argv[n], len+1);
    return(1);
  } else {
    memcpy(buf, argv[n], buflen - 1);
    buf[buflen] = '\0';
    return(0);
  }
}


// tx, rx
// Serial1 1/2 (GP0/1)
// Serial2 6/7 (GP4/5)
SerialPIO Serial3(8, 9, READBUFSIZE); // 11/12 (GP8/9)
SerialPIO Serial4(12, 13, READBUFSIZE); // 16/17 (GP12/13)
SerialPIO Serial5(16, 17, READBUFSIZE); // 21/22 (GP16/17)
SerialPIO Serial6(20, 21, READBUFSIZE); // 26/27 (GP20/21)
Adafruit_USBD_CDC USB1;
Adafruit_USBD_CDC USB2;
Adafruit_USBD_CDC USB3;
Adafruit_USBD_CDC USB4;
Adafruit_USBD_CDC USB5;
Adafruit_USBD_CDC USB6;

UsbUartBridge UUB[UUB_MAX];
CliParser cli(CMDLINE_ARGC_MAX, CMDLINE_CHAR_MAX);

void setup() {
  Serial1.setTX(0);
  Serial1.setRX(1);
  Serial2.setTX(4);
  Serial2.setRX(5);

  Serial.begin(115200);
  UUB[0].init((char *)"B0 (GP0/1)", &USB1, &Serial1, 115200);
  UUB[1].init((char *)"B1 (GP4/5)", &USB2, &Serial2, 115200);
  UUB[2].init((char *)"B2 (GP8/9)", &USB3, &Serial3, 115200);
  UUB[3].init((char *)"B3 (GP12/13)", &USB4, &Serial4, 115200);
  UUB[4].init((char *)"B4 (GP16/17)", &USB5, &Serial5, 115200);
  UUB[5].init((char *)"B5 (GP20/21)", &USB6, &Serial6, 115200);

  Ascii::Init();
}

void loop() {
  int i, argc;

  while(1) {
    for(i = 0; i < UUB_MAX; i++) {
      UUB[i].transmit();
    }

    if(cli.getCmdStr(&Serial)) {
      argc = cli.parseCmdStr();
      //cli.debugPrint(&Serial);

      if(argc > 0) {
        if(cli.argvCmp(0, "cap") == 0 && argc > 1) {
          Serial.println("Capture:");

          int n = cli.getArgv(1);
          if(n >= 0 && n < UUB_MAX) {
            Serial.printf("  enable for %s\n", UUB[n].getDescr());
            UUB[n].setCapture(&Serial);
          } else {
            Serial.printf("  invalid serial port number %d\n", n);
          }

        } else if(cli.argvCmp(0, "uncap") == 0 && argc > 1) {
          Serial.println("Uncapture:");
          int n = cli.getArgv(1);
          if(n >= 0 && n < UUB_MAX) {
            Serial.printf("  disable for %s\n", UUB[n].getDescr());
            UUB[n].setCapture(NULL);
            UUB[n].initCapture();
          } else {
            Serial.printf("  invalid serial port number %d\n", n);
          }

        } else if(cli.argvCmp(0, "capmode") == 0 && argc > 2) {
          Serial.println("Cpature mode:");
          int n = cli.getArgv(1);
          int mode = cli.getArgv(2);
          if(n >= 0 && n < UUB_MAX) {
            mode = UUB[n].setCapMode(mode);
            Serial.printf("  %s capture mode to %d\n", UUB[n].getDescr(), mode);
            UUB[n].initCapture();
          } else {
            Serial.printf("  invalid serial port number %d\n", n);
          }

        } else if(cli.argvCmp(0, "capdelim") ==0 && argc > 2) {
          Serial.println("Capture Delimiter:");
          char dstr[3];

          int n = cli.getArgv(1);
          cli.getArgv(2, dstr, sizeof(dstr));

          if(n >= 0 && n < UUB_MAX) {
            unsigned char d;
            if(dstr[0] >= '0' && dstr[0] <= '9') {
              d = (char)cli.getArgv(2);
            } else {
              d = Ascii::Chr(dstr);
            }
            Serial.printf("  %s cap delim %s(%d)\n", UUB[n].getDescr(), Ascii::Str(d), d);
            UUB[n].setCapDelim(d);
            UUB[n].initCapture();
          } else {
            Serial.printf("  invalid serial port number %d\n", n);
          }

        } else if(cli.argvCmp(0, "show") == 0) {
          Serial.println("Show:");
          Serial.printf("  %-13s | %7s | %-7s | %-7s | %-9s | %10s | %10s\n",
            "bridge", "baudrt", "capture",
            "capmode",
            "capdelim", "usb->uart", "uart->usb");
          int capmode;
          unsigned char delim;
          for(int i = 0; i < UUB_MAX; i++) {
            capmode = UUB[i].getCapMode();
            delim = UUB[i].getCapDelim();
            Serial.printf("  %-13s | % 7d | %-7s | %-3s %-3s | %-4s(% 3d) | % 10d | % 10d\n",
              UUB[i].getDescr(), UUB[i].getBaud(), (UUB[i].getCapture() ? "on" : "off"),
              (capmode & CAPMODE_TXT ? "TXT" : ""), (capmode & CAPMODE_HEX ? "HEX" : ""),
              Ascii::Str(delim), delim, UUB[i].getUsb2Uart(), UUB[i].getUart2Usb());
          }

        } else if(cli.argvCmp(0, "clear") == 0 && argc > 1) {
          int n;
          if(cli.argvCmp(1, "all") == 0) {
            for(i = 0; i < UUB_MAX; i++) {
              UUB[i].clearCnt();
            }
          } else {
            int n = cli.getArgv(1);
            if(n >= 0 && n < UUB_MAX) UUB[n].clearCnt();
            else Serial.printf("invalid serial number %d\n", n);
          }

        } else if(cli.argvCmp(0, "help") == 0){
          Serial.println("Usage: ");
          Serial.println("  show             : show current parameters");
          Serial.println("  cap num          : enable capture for bridge<n>");
          Serial.println("  uncap num        : disable capture for bridge<num>");
          Serial.println("  capmode num mode : set capture mode for bridge<num> to <mode>");
          Serial.println("                   : mode: 1=TXT, 2=HEX, 3=TXT&HEX");
          Serial.println("  capdelim num chr : set TXT capture delimiter for bridge<num>");
          Serial.println("                   : chr: decimal number from 0 to 255 or");
          Serial.println("                   :      \\0,\\b,\\t,\\n(default),\\v,\\f,\\r, or");
          Serial.println("                   :      signle character");
          Serial.println("  clear {num | all}: clear bytes count");
          Serial.println("  help             : print this help");

        } else {
          // just echo
          Serial.print("NOP:");
          char buf[CMDLINE_CHAR_MAX + 1];
          for(i = 0; i < argc; i++) {
            cli.getArgv(i, buf, sizeof(buf));
            Serial.print(buf);
            Serial.print(" ");
          }
          Serial.println();
        }
      }

      cli.initCmdStr();
    }
  }
}
