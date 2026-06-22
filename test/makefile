## makefile: helios_test #######################################
##
## Unit tests for the heliosAgent dispatch + verb logic. Compiles the
## app's Dispatch.cpp/Verbs.cpp alongside the test and drives
## heliosDispatch() directly (no socket). Run from the app dir with
## `make test`, or here with `make && ./<objdir>/helios_test`.

APP=        helios_test
CPP=        g++
SHELL=      /bin/sh
RM=         rm -rf
INC=        -I../../.. -I..

## Platform ###################################################

UNAME_S := $(shell uname -s | tr '[A-Z]' '[a-z]' )

ifeq ($(UNAME_S),linux)
	ARCH := $(shell uname -m | tr '[A-Z]' '[a-z]' )
	CPPFLAGS = -D _LINUX_  -g -Wno-deprecated
endif

ifeq ($(UNAME_S), darwin)
	ARCH := $(shell uname -m | tr '[A-Z]' '[a-z]' )
	CPPFLAGS = -D _OSX_ -g -Wno-deprecated
endif

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

    PLATFORM_LIBS = -lsocket -lnsl
endif

ifeq ($(UNAME_S),irix)
    UNAME_R := $(shell uname -r)
    ifeq ($(UNAME_R), 6.5)
        CPPFLAGS = -D _IRIX6_ -g
    endif
endif

ifeq ($(UNAME_S),netbsd)
	CPPFLAGS = -D _NETBSD_ -g
endif

## Object & Libraries #########################################

LIB_CX_PLATFORM_LIB_DIR=../../../lib/$(UNAME_S)_$(ARCH)

APP_OBJECT_DIR=$(UNAME_S)_$(ARCH)

LIB_CX_BASE_NAME=libcx_base.a
LIB_CX_JSON_NAME=libcx_json.a
LIB_CX_B64_NAME=libcx_b64.a
LIB_CX_PROCESS_NAME=libcx_process.a
LIB_CX_NET_NAME=libcx_net.a

# Base library must come last - the other libs depend on it.
CX_LIBS = \
	$(LIB_CX_PLATFORM_LIB_DIR)/$(LIB_CX_JSON_NAME)    \
	$(LIB_CX_PLATFORM_LIB_DIR)/$(LIB_CX_B64_NAME)     \
	$(LIB_CX_PLATFORM_LIB_DIR)/$(LIB_CX_PROCESS_NAME) \
	$(LIB_CX_PLATFORM_LIB_DIR)/$(LIB_CX_NET_NAME)     \
	$(LIB_CX_PLATFORM_LIB_DIR)/$(LIB_CX_BASE_NAME)

ALL_LIBS = $(CX_LIBS) $(PLATFORM_LIBS)

# The test links the app's logic objects (Dispatch + Verbs), compiled here
# from the parent dir, plus the test itself.
OBJECTS = \
	$(APP_OBJECT_DIR)/helios_test.o \
	$(APP_OBJECT_DIR)/Dispatch.o    \
	$(APP_OBJECT_DIR)/Verbs.o

## Targets ####################################################

ALL: MAKE_OBJ_DIR $(APP_OBJECT_DIR)/$(APP)

MAKE_OBJ_DIR:
	test -d $(APP_OBJECT_DIR) || mkdir $(APP_OBJECT_DIR)

cleanupmac:
	$(RM) ._*

clean:
	$(RM) \
	$(APP_OBJECT_DIR)/$(APP) \
	$(APP_OBJECT_DIR)/*.o   \
	$(APP_OBJECT_DIR)/core  \
	$(APP_OBJECT_DIR)/a.out

$(APP_OBJECT_DIR)/$(APP): $(OBJECTS)
	$(CPP) $(CPPFLAGS) $(INC) $(OBJECTS) -o $(APP_OBJECT_DIR)/$(APP) $(ALL_LIBS)
ifeq ($(UNAME_S), darwin)
	xattr -cr $(APP_OBJECT_DIR)/$(APP)
endif

## Conversions ################################################

$(APP_OBJECT_DIR)/helios_test.o : helios_test.cpp
$(APP_OBJECT_DIR)/Dispatch.o    : ../Dispatch.cpp
$(APP_OBJECT_DIR)/Verbs.o       : ../Verbs.cpp

.PRECIOUS: $(CX_LIBS)
.SUFFIXES: .cpp .C .cc .cxx .o

$(OBJECTS):
	$(CPP) $(CPPFLAGS) $(INC) -c $? -o $@

run: ALL
	./$(APP_OBJECT_DIR)/$(APP)
