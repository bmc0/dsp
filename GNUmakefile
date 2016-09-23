DEPS := config.mk dsp.h
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
	delay.o \
	noise.o \
	compress.o \
	stats.o \
	null.o \
	pcm.o
DSP_CPP_OBJ :=
LADSPA_DSP_OBJ := ladspa_dsp.o \
	effect.o \
	util.o \
	biquad.o \
	gain.o \
	crossfeed.o \
	remix.o \
	delay.o \
	noise.o \
	compress.o \
	stats.o
LADSPA_DSP_CPP_OBJ :=

BASE_CFLAGS        := -Os -Wall -std=gnu99
BASE_CXXFLAGS      := -Os -Wall -std=gnu++11
BASE_LDFLAGS       :=
BASE_LIBS          := -lm

include config.mk

DSP_CFLAGS          := ${BASE_CFLAGS} ${DSP_EXTRA_CFLAGS} ${CFLAGS}
DSP_CXXFLAGS        := ${BASE_CXXFLAGS} ${DSP_EXTRA_CFLAGS} ${CXXFLAGS}
DSP_LDFLAGS         := ${BASE_LDFLAGS} ${LDFLAGS}
DSP_LIBS            := ${DSP_EXTRA_LIBS} ${BASE_LIBS}
LADSPA_DSP_CFLAGS   := ${BASE_CFLAGS} -fPIC -DPIC -D__SYMMETRIC_IO__ ${LADSPA_DSP_EXTRA_CFLAGS} ${CFLAGS}
LADSPA_DSP_CXXFLAGS := ${BASE_CXXFLAGS} -fPIC -DPIC -D__SYMMETRIC_IO__ ${LADSPA_DSP_EXTRA_CFLAGS} ${CXXFLAGS}
LADSPA_DSP_LDFLAGS  := ${BASE_LDFLAGS} -shared -nostartfiles -fPIC ${LDFLAGS}
LADSPA_DSP_LIBS     := ${LADSPA_DSP_EXTRA_LIBS} ${BASE_LIBS}
DSP_OBJ             := ${addprefix ${DSP_OBJDIR}/,${DSP_OBJ}}
DSP_CPP_OBJ         := ${addprefix ${DSP_OBJDIR}/,${DSP_CPP_OBJ}}
LADSPA_DSP_OBJ      := ${addprefix ${LADSPA_DSP_OBJDIR}/,${LADSPA_DSP_OBJ}}
LADSPA_DSP_CPP_OBJ  := ${addprefix ${LADSPA_DSP_OBJDIR}/,${LADSPA_DSP_CPP_OBJ}}

ladspa_dsp: ladspa_dsp.so

config.mk: configure
	./configure

${DSP_OBJDIR} ${LADSPA_DSP_OBJDIR}:
	mkdir -p $@

${DSP_OBJ}: ${DSP_OBJDIR}/%.o: %.c ${DEPS} | ${DSP_OBJDIR}
	${CC} -c -o $@ ${DSP_CFLAGS} $<

${DSP_CPP_OBJ}: ${DSP_OBJDIR}/%.o: %.cpp ${DEPS} | ${DSP_OBJDIR}
	${CXX} -c -o $@ ${DSP_CXXFLAGS} $<

${LADSPA_DSP_OBJ}: ${LADSPA_DSP_OBJDIR}/%.o: %.c ${DEPS} | ${LADSPA_DSP_OBJDIR}
	${CC} -c -o $@ ${LADSPA_DSP_CFLAGS} $<

${LADSPA_DSP_CPP_OBJ}: ${LADSPA_DSP_OBJDIR}/%.o: %.cpp ${DEPS} | ${LADSPA_DSP_OBJDIR}
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

install_ladspa_dsp: ladspa_dsp.so
	install -Dm755 ladspa_dsp.so ${DESTDIR}${PREFIX}${LIBDIR}/ladspa/ladspa_dsp.so

clean:
	rm -f dsp ladspa_dsp.so ${DSP_OBJ} ${DSP_CPP_OBJ} ${LADSPA_DSP_OBJ} ${LADSPA_DSP_CPP_OBJ}

distclean: clean
	rm -f config.mk
	rm -rf ${OBJDIR}

.PHONY: all install ladspa_dsp install_dsp install_ladspa_dsp clean distclean
