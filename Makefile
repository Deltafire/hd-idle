###############################################################################
#
# General Definitions
#
###############################################################################

ifdef DESTDIR
# dh_auto_install (Debian) sets this variable
  TARGET_DIR = $(DESTDIR)/usr
else
  TARGET_DIR ?= /usr/local
endif

LIB_DIRS   = 

INC_DIRS   = 

CC        ?= gcc
CFLAGS    += $(INC_DIRS) -Wall

LD         = $(CC)
LDFLAGS   += $(LIB_DIRS)

###############################################################################
#
# Main Dependencies
#
###############################################################################

TARGET  = hd-idle

LIBS    = 

SRCS    = hd-idle.c

OBJS    = $(SRCS:.c=.o)

all: $(TARGET)

distclean: clean

clean:
	rm -f $(OBJS) $(TARGET)

install: $(TARGET)
	install -D $(TARGET) $(TARGET_DIR)/sbin/$(TARGET)
	install -d $(TARGET_DIR)/share/man/man1/
	install -p -m 644 $(TARGET).1 $(TARGET_DIR)/share/man/man1/$(TARGET).1

hd-idle.o:     hd-idle.c

$(TARGET): $(OBJS)
	$(LD) $(LDFLAGS) -o $(TARGET) $(OBJS) $(LIB_DIRS) $(LIBS)


