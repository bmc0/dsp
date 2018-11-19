STATIC_DEPS := config.mk
OBJDIR := obj
DSP_OBJDIR := ${OBJDIR}/dsp
LADSPA_DSP_OBJDIR := ${OBJDIR}/ladspa_dsp
DSP_OBJ := dsp.o \
	effect.o \
	codec.o \
	sampleconv.o \
	util.o \
	biquad.o \
	gain.o \
	crossfeed.o \
	remix.o \
	st2ms.o \
	delay.o \
	noise.o \
	stats.o \
	null.o \
	sgen.o \
	pcm.o
DSP_CPP_OBJ :=
LADSPA_DSP_OBJ := ladspa_dsp.o \
	effect.o \
	util.o \
	biquad.o \
	gain.o \
	crossfeed.o \
	remix.o \
	st2ms.o \
	delay.o \
	noise.o \
	stats.o
LADSPA_DSP_CPP_OBJ :=

BASE_CFLAGS        := -Os -Wall -std=gnu99
BASE_CXXFLAGS      := -Os -Wall -std=gnu++11
BASE_LDFLAGS       :=
BASE_LIBS          := -lm

include config.mk

DEPFLAGS            := -MMD -MP
DSP_CFLAGS          := ${DEPFLAGS} ${BASE_CFLAGS} ${DSP_EXTRA_CFLAGS} ${CFLAGS} ${CPPFLAGS}
DSP_CXXFLAGS        := ${DEPFLAGS} ${BASE_CXXFLAGS} ${DSP_EXTRA_CFLAGS} ${CXXFLAGS} ${CPPFLAGS}
DSP_LDFLAGS         := ${BASE_LDFLAGS} ${LDFLAGS}
DSP_LIBS            := ${DSP_EXTRA_LIBS} ${BASE_LIBS}
LADSPA_DSP_CFLAGS   := ${DEPFLAGS} ${BASE_CFLAGS} -fPIC -DPIC -DLADSPA_FRONTEND -DSYMMETRIC_IO ${LADSPA_DSP_EXTRA_CFLAGS} ${CFLAGS} ${CPPFLAGS}
LADSPA_DSP_CXXFLAGS := ${DEPFLAGS} ${BASE_CXXFLAGS} -fPIC -DPIC -DLADSPA_FRONTEND -DSYMMETRIC_IO ${LADSPA_DSP_EXTRA_CFLAGS} ${CXXFLAGS} ${CPPFLAGS}
LADSPA_DSP_LDFLAGS  := ${BASE_LDFLAGS} -shared -fPIC ${LDFLAGS}
LADSPA_DSP_LIBS     := ${LADSPA_DSP_EXTRA_LIBS} ${BASE_LIBS} -lc
DSP_OBJ             := ${addprefix ${DSP_OBJDIR}/,${DSP_OBJ}}
DSP_CPP_OBJ         := ${addprefix ${DSP_OBJDIR}/,${DSP_CPP_OBJ}}
DSP_DEPFILES        := ${patsubst %.o,%.d,${DSP_OBJ} ${DSP_CPP_OBJ}}
LADSPA_DSP_OBJ      := ${addprefix ${LADSPA_DSP_OBJDIR}/,${LADSPA_DSP_OBJ}}
LADSPA_DSP_CPP_OBJ  := ${addprefix ${LADSPA_DSP_OBJDIR}/,${LADSPA_DSP_CPP_OBJ}}
LADSPA_DSP_DEPFILES := ${patsubst %.o,%.d,${LADSPA_DSP_OBJ} ${LADSPA_DSP_CPP_OBJ}}

ladspa_dsp: ladspa_dsp.so

config.mk: configure
	./configure

${DSP_OBJDIR} ${LADSPA_DSP_OBJDIR}:
	mkdir -p $@

${DSP_OBJ}: ${DSP_OBJDIR}/%.o: %.c ${STATIC_DEPS} | ${DSP_OBJDIR}
	${CC} -c -o $@ ${DSP_CFLAGS} $<

${DSP_CPP_OBJ}: ${DSP_OBJDIR}/%.o: %.cpp ${STATIC_DEPS} | ${DSP_OBJDIR}
	${CXX} -c -o $@ ${DSP_CXXFLAGS} $<

${LADSPA_DSP_OBJ}: ${LADSPA_DSP_OBJDIR}/%.o: %.c ${STATIC_DEPS} | ${LADSPA_DSP_OBJDIR}
	${CC} -c -o $@ ${LADSPA_DSP_CFLAGS} $<

${LADSPA_DSP_CPP_OBJ}: ${LADSPA_DSP_OBJDIR}/%.o: %.cpp ${STATIC_DEPS} | ${LADSPA_DSP_OBJDIR}
	${CXX} -c -o $@ ${LADSPA_DSP_CXXFLAGS} $<

ifdef DSP_CPP_OBJ
dsp: ${DSP_OBJ} ${DSP_CPP_OBJ}
	${CXX} -o $@ ${DSP_LDFLAGS} ${DSP_OBJ} ${DSP_CPP_OBJ} ${DSP_LIBS}
else
dsp: ${DSP_OBJ}
	${CC} -o $@ ${DSP_LDFLAGS} ${DSP_OBJ} ${DSP_LIBS}
endif

ifdef LADSPA_DSP_CPP_OBJ
ladspa_dsp.so: ${LADSPA_DSP_OBJ} ${LADSPA_DSP_CPP_OBJ}
	${CXX} -o $@ ${LADSPA_DSP_LDFLAGS} ${LADSPA_DSP_OBJ} ${LADSPA_DSP_CPP_OBJ} ${LADSPA_DSP_LIBS}
else
ladspa_dsp.so: ${LADSPA_DSP_OBJ}
	${CC} -o $@ ${LADSPA_DSP_LDFLAGS} ${LADSPA_DSP_OBJ} ${LADSPA_DSP_LIBS}
endif

install_dsp: dsp
	install -Dm755 dsp ${DESTDIR}${PREFIX}${BINDIR}/dsp

uninstall_dsp:
	rm -f ${DESTDIR}${PREFIX}${BINDIR}/dsp

install_ladspa_dsp: ladspa_dsp.so
	install -Dm755 ladspa_dsp.so ${DESTDIR}${PREFIX}${LIBDIR}/ladspa/ladspa_dsp.so

uninstall_ladspa_dsp:
	rm -f ${DESTDIR}${PREFIX}${LIBDIR}/ladspa/ladspa_dsp.so

install_manual:
	install -Dm644 dsp.1 ${DESTDIR}${PREFIX}${DATADIR}${MANDIR}/man1/dsp.1

uninstall_manual:
	rm -f ${DESTDIR}${PREFIX}${DATADIR}${MANDIR}/man1/dsp.1

clean:
	rm -f dsp ladspa_dsp.so ${DSP_OBJ} ${DSP_CPP_OBJ} ${DSP_DEPFILES} ${LADSPA_DSP_OBJ} ${LADSPA_DSP_CPP_OBJ} ${LADSPA_DSP_DEPFILES}

distclean: clean
	rm -f config.mk
	rm -rf ${OBJDIR}

.PHONY: all install uninstall ladspa_dsp install_dsp uninstall_dsp install_ladspa_dsp uninstall_ladspa_dsp install_manual uninstall_manual clean distclean

-include ${DSP_DEPFILES} ${LADSPA_DSP_DEPFILES}
