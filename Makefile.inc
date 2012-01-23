SRCS+= thashmap.c thashmap.h
.PATH: ${.CURDIR}/..

CFLAGS+= -I${.CURDIR}/..

WARNS=6
DEBUG_FLAGS+=-g
NO_MAN=
