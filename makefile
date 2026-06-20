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

# get the OS
UNAME_S := $(shell uname -s | tr '[A-Z]' '[a-z]' )

# if this is linux
ifeq ($(UNAME_S),linux)
	ARCH := $(shell uname -m | tr '[A-Z]' '[a-z]' )
	CPPFLAGS = -D _LINUX_  -g -Wno-deprecated
endif

#if this is OSX
ifeq ($(UNAME_S), darwin)
	ARCH := $(shell uname -m | tr '[A-Z]' '[a-z]' )
	CPPFLAGS = -D _OSX_ -g -Wno-deprecated
endif

#if this is SUNOS or SOLARIS
ifeq ($(UNAME_S),sunos)
    UNAME_R := $(shell uname -r)

    ifeq ($(UNAME_R), 4.1.3)
        CPPFLAGS = -D _SUNOS_ -g
    endif

    ifeq ($(UNAME_R), 4.1.4)
        CPPFLAGS = -D _SUNOS_ -g
    endif

    ifeq ($(UNAME_R), 5.6)
        CPPFLAGS = -D _SOLARIS6_ -g
    endif

    ifeq ($(UNAME_R), 5.7)
        CPPFLAGS = -D _SOLARIS6_ -g
    endif

    ifeq ($(UNAME_R), 5.10)
        CPPFLAGS = -D _SOLARIS10_ -g
    endif

    # Solaris network functions need these explicitly.
    PLATFORM_LIBS = -lsocket -lnsl
endif

#if this is IRIX
ifeq ($(UNAME_S),irix)
    UNAME_R := $(shell uname -r)
    ifeq ($(UNAME_R), 6.5)
        CPPFLAGS = -D _IRIX6_ -g
    endif
endif

#if this is NETBSD
ifeq ($(UNAME_S),netbsd)
	CPPFLAGS = -D _NETBSD_ -g
endif

## Object & Libraries #########################################

LIB_CX_PLATFORM_LIB_DIR=../../lib/$(UNAME_S)_$(ARCH)

APP_OBJECT_DIR=$(UNAME_S)_$(ARCH)

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

archive:
	@echo "Creating heliosagent_unix.tar..."
	@test -d ../../ARCHIVE || mkdir ../../ARCHIVE
	@cd ../.. && tar cvf ARCHIVE/heliosagent_unix.tar \
		--exclude='._*' \
		--exclude='*.o' \
		--exclude='*.a' \
		--exclude='.git' \
		--exclude='.claude' \
		--exclude='.DS_Store' \
		--exclude='darwin_*' \
		--exclude='linux_*' \
		--exclude='sunos_*' \
		--exclude='irix_*' \
		--exclude='netbsd_*' \
		cx_apps/heliosAgent
	@echo "Archive created: ../../ARCHIVE/heliosagent_unix.tar"
