## Makefile: heliosAgent ######################################
##
## The Helios guest agent: a small TCP daemon that proxies command
## execution and filesystem access to a Mac-side client over one
## newline-delimited JSON channel. Built on cx, so it compiles the same
## on macOS (dev), Linux, and Solaris (deploy). Structure mirrors
## cx_apps/cm. See PROTOCOL.md.

CPP=        g++
SHELL=      /bin/sh
MOVE=       mv
COPY=       ln -s
RM=         rm -rf
TOUCH=      touch
LIBRARIAN=  ar rvu
INC=        -I../..

## Platform ###################################################

# Platform detection + flags live in cx/platform.mk (single source of truth).
include ../../cx/platform.mk

## Object & Libraries #########################################

LIB_CX_PLATFORM_LIB_DIR=../../lib/$(PLATFORM)

APP_OBJECT_DIR=$(PLATFORM)

LIB_CX_BASE_NAME=libcx_base.a
LIB_CX_NET_NAME=libcx_net.a
LIB_CX_JSON_NAME=libcx_json.a
LIB_CX_B64_NAME=libcx_b64.a
LIB_CX_LOG_NAME=libcx_log.a
LIB_CX_PROCESS_NAME=libcx_process.a

# Base library must come last - the other libs depend on it.
CX_LIBS = \
	$(LIB_CX_PLATFORM_LIB_DIR)/$(LIB_CX_NET_NAME)     \
	$(LIB_CX_PLATFORM_LIB_DIR)/$(LIB_CX_JSON_NAME)    \
	$(LIB_CX_PLATFORM_LIB_DIR)/$(LIB_CX_B64_NAME)     \
	$(LIB_CX_PLATFORM_LIB_DIR)/$(LIB_CX_LOG_NAME)     \
	$(LIB_CX_PLATFORM_LIB_DIR)/$(LIB_CX_PROCESS_NAME)

CX_LIBS_BASE = $(LIB_CX_PLATFORM_LIB_DIR)/$(LIB_CX_BASE_NAME)

ALL_LIBS = $(CX_LIBS) $(CX_LIBS_BASE) $(PLATFORM_LIBS)

OBJECTS = \
	$(APP_OBJECT_DIR)/HeliosAgent.o \
	$(APP_OBJECT_DIR)/Dispatch.o    \
	$(APP_OBJECT_DIR)/Verbs.o

## Targets ####################################################

ALL: MAKE_OBJ_DIR $(APP_OBJECT_DIR)/heliosAgent

MAKE_OBJ_DIR:
	test -d $(APP_OBJECT_DIR) || mkdir $(APP_OBJECT_DIR)

cleanupmac:
	$(RM) ._*

clean:
	$(RM) \
	$(APP_OBJECT_DIR)/heliosAgent \
	$(APP_OBJECT_DIR)/*.o   \
	$(APP_OBJECT_DIR)/*.dbx \
	$(APP_OBJECT_DIR)/core  \
	$(APP_OBJECT_DIR)/a.out \
	$(APP_OBJECT_DIR)/*.a

install:
ifeq ($(UNAME_S), sunos)
	cp $(APP_OBJECT_DIR)/heliosAgent /usr/local/bin/heliosAgent
	chmod 755 /usr/local/bin/heliosAgent
endif
ifeq ($(UNAME_S), darwin)
	sudo cp $(APP_OBJECT_DIR)/heliosAgent /usr/local/bin/heliosAgent
	sudo chmod 755 /usr/local/bin/heliosAgent
	sudo xattr -cr /usr/local/bin/heliosAgent
endif

$(APP_OBJECT_DIR)/heliosAgent: $(OBJECTS)
	$(CPP) $(CPPFLAGS) $(INC) $(OBJECTS) -o $(APP_OBJECT_DIR)/heliosAgent $(ALL_LIBS)
ifeq ($(UNAME_S), darwin)
	xattr -cr $(APP_OBJECT_DIR)/heliosAgent
endif

## Conversions ################################################

$(APP_OBJECT_DIR)/HeliosAgent.o : HeliosAgent.cpp
$(APP_OBJECT_DIR)/Dispatch.o    : Dispatch.cpp
$(APP_OBJECT_DIR)/Verbs.o       : Verbs.cpp

.PRECIOUS: $(CX_LIBS)
.SUFFIXES: .cpp .C .cc .cxx .o

$(OBJECTS):
	$(CPP) $(CPPFLAGS) $(INC) -c $? -o $@

## Tests ######################################################

# Unit tests live with the app (it's a cx app, not a cx library module, so it
# is not under cx_tests). They drive heliosDispatch() directly. See test/.
.PHONY: test
test:
	cd test && make
	test/$(APP_OBJECT_DIR)/helios_test

testclean:
	cd test && make clean

## Archive ####################################################

# Archives are built from the umbrella makefile (../../makefile), not here.
# heliosAgent ships inside cxapps_unix.tar (with cm, psd):
#   make -C ../.. cxapps_unix.tar
# One place to maintain the object/binary exclusion.
