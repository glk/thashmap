SUBDIR= test bench

run: ${SUBDIR}
.for DIR in ${SUBDIR}
	${.OBJDIR}/${DIR}/thashmap-${DIR}
.endfor

.include <bsd.subdir.mk>
