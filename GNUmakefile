DEPS := config.mk dsp.h
OBJDIR := obj
DSP_OBJDIR := ${OBJDIR}/dsp
LADSPA_DSP_OBJDIR := ${OBJDIR}/ladspa_dsp
DSP_OBJ := dsp.o \
	effect.o \
	codec.o \
	dither.o \
	sampleconv.o \
	util.o \
	biquad.o \
	gain.o \
	crossfeed.o \
	remix.o \
	delay.o \
	sndfile.o \
	null.o \
	pcm.o
LADSPA_DSP_OBJ := ladspa_dsp.o \
	effect.o \
	util.o \
	biquad.o \
	gain.o \
	crossfeed.o \
	remix.o \
	delay.o

BASE_CFLAGS        := -Os -Wall -std=gnu99
BASE_LDFLAGS       := -lm

include config.mk

DSP_CFLAGS         := ${BASE_CFLAGS} ${DSP_EXTRA_CFLAGS} ${CFLAGS}
DSP_LDFLAGS        := ${BASE_LDFLAGS} ${DSP_EXTRA_LDFLAGS} ${LDFLAGS}
LADSPA_DSP_CFLAGS  := ${BASE_CFLAGS} -fPIC -DPIC ${CFLAGS}
LADSPA_DSP_LDFLAGS := ${BASE_LDFLAGS} -shared -nostartfiles ${LDFLAGS}
DSP_OBJ            := ${addprefix ${DSP_OBJDIR}/,${DSP_OBJ}}
LADSPA_DSP_OBJ     := ${addprefix ${LADSPA_DSP_OBJDIR}/,${LADSPA_DSP_OBJ}}

ladspa_dsp: ladspa_dsp.so

config.mk: configure
	./configure

${DSP_OBJDIR} ${LADSPA_DSP_OBJDIR}:
	mkdir -p $@

${DSP_OBJ}: ${DSP_OBJDIR}/%.o: %.c ${DEPS} | ${DSP_OBJDIR}
	${CC} -c -o $@ ${DSP_CFLAGS} $<

${LADSPA_DSP_OBJ}: ${LADSPA_DSP_OBJDIR}/%.o: %.c ${DEPS} | ${LADSPA_DSP_OBJDIR}
	${CC} -c -o $@ ${LADSPA_DSP_CFLAGS} $<

dsp: ${DSP_OBJ}
	${CC} -o $@ ${DSP_LDFLAGS} ${DSP_OBJ}

ladspa_dsp.so: ${LADSPA_DSP_OBJ}
	${CC} -o $@ ${LADSPA_DSP_LDFLAGS} ${LADSPA_DSP_OBJ}

clean:
	rm -f dsp ladspa_dsp.so ${DSP_OBJ} ${LADSPA_DSP_OBJ}

distclean: clean
	rm -f config.mk
	rm -fd ${DSP_OBJDIR} ${LADSPA_DSP_OBJDIR} ${OBJDIR}

.PHONY: all ladspa_dsp clean distclean
