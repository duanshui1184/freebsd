/*-
 * Copyright (c) 2012 The FreeBSD Foundation
 * All rights reserved.
 *
 * This software was developed by Oleksandr Rybalko under sponsorship
 * from the FreeBSD Foundation.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1.	Redistributions of source code must retain the above copyright
 *	notice, this list of conditions and the following disclaimer.
 * 2.	Redistributions in binary form must reproduce the above copyright
 *	notice, this list of conditions and the following disclaimer in the
 *	documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/malloc.h>
#include <sys/rman.h>
#include <sys/timeet.h>
#include <sys/timetc.h>
#include <sys/watchdog.h>
#include <machine/bus.h>
#include <machine/cpu.h>
#include <machine/frame.h>
#include <machine/intr.h>

#include <dev/fdt/fdt_common.h>
#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include <machine/bus.h>
#include <machine/fdt.h>
#include <arm/freescale/imx/imx_gptvar.h>
#include <arm/freescale/imx/imx_gptreg.h>

#include <sys/kdb.h>
#include <arm/freescale/imx/imx51_ccmvar.h>

#define	WRITE4(_sc, _r, _v)						\
	    bus_space_write_4((_sc)->sc_iot, (_sc)->sc_ioh, (_r), (_v))
#define	READ4(_sc, _r)							\
	    bus_space_read_4((_sc)->sc_iot, (_sc)->sc_ioh, (_r))
#define	SET4(_sc, _r, _m)						\
	    WRITE4((_sc), (_r), READ4((_sc), (_r)) | (_m))
#define	CLEAR4(_sc, _r, _m)						\
	    WRITE4((_sc), (_r), READ4((_sc), (_r)) & ~(_m))

static u_int	imx_gpt_get_timecount(struct timecounter *);
static int	imx_gpt_timer_start(struct eventtimer *,
    struct bintime *, struct bintime *);
static int	imx_gpt_timer_stop(struct eventtimer *);

static int imx_gpt_intr(void *);
static int imx_gpt_probe(device_t);
static int imx_gpt_attach(device_t);

static struct timecounter imx_gpt_timecounter = {
	.tc_name           = "i.MX GPT Timecouter",
	.tc_get_timecount  = imx_gpt_get_timecount,
	.tc_counter_mask   = ~0u,
	.tc_frequency      = 0,
	.tc_quality        = 500,
};

struct imx_gpt_softc *imx_gpt_sc = NULL;
static volatile int imx_gpt_delay_count = 300;

static struct resource_spec imx_gpt_spec[] = {
	{ SYS_RES_MEMORY,	0,	RF_ACTIVE },
	{ SYS_RES_IRQ,		0,	RF_ACTIVE },
	{ -1, 0 }
};

static int
imx_gpt_probe(device_t dev)
{

	if (!ofw_bus_is_compatible(dev, "fsl,imx51-gpt"))
		return (ENXIO);

	device_set_desc(dev, "Freescale i.MXxxx GPT timer");
	return (BUS_PROBE_DEFAULT);
}

static int
imx_gpt_attach(device_t dev)
{
	struct imx_gpt_softc *sc;
	int err;

	sc = device_get_softc(dev);

	if (bus_alloc_resources(dev, imx_gpt_spec, sc->res)) {
		device_printf(dev, "could not allocate resources\n");
		return (ENXIO);
	}

	sc->sc_dev = dev;
	sc->sc_clksrc = GPT_CR_CLKSRC_IPG;
	sc->sc_iot = rman_get_bustag(sc->res[0]);
	sc->sc_ioh = rman_get_bushandle(sc->res[0]);

	switch (sc->sc_clksrc) {
	case GPT_CR_CLKSRC_NONE:
		device_printf(dev, "can't run timer without clock source\n");
		return (EINVAL);
	case GPT_CR_CLKSRC_EXT:
		device_printf(dev, "Not implemented. Geve me the way to get "
		    "external clock source frequency\n");
		return (EINVAL);
	case GPT_CR_CLKSRC_32K:
		sc->clkfreq = 32768;
		break;
	case GPT_CR_CLKSRC_IPG_HIGH:
		sc->clkfreq = imx51_get_clock(IMX51CLK_IPG_CLK_ROOT) * 2;
		break;
	default:
		sc->clkfreq = imx51_get_clock(IMX51CLK_IPG_CLK_ROOT);
	}
	device_printf(dev, "Run on %dKHz clock.\n", sc->clkfreq / 1000);

	/* Reset */
	WRITE4(sc, IMX_GPT_CR, GPT_CR_SWR);
	/* Enable and setup counters */
	WRITE4(sc, IMX_GPT_CR,
	    GPT_CR_CLKSRC_IPG |	/* Use IPG clock */
	    GPT_CR_FRR |	/* Just count (FreeRunner mode) */
	    GPT_CR_STOPEN |	/* Run in STOP mode */
	    GPT_CR_WAITEN |	/* Run in WAIT mode */
	    GPT_CR_DBGEN);	/* Run in DEBUG mode */

	/* Disable interrupts */
	WRITE4(sc, IMX_GPT_IR, 0);

	/* Tick every 10us */
	/* XXX: must be calculated from clock source frequency */
	WRITE4(sc, IMX_GPT_PR, 665);
	/* Use 100 KHz */
	sc->clkfreq = 100000;

	/* Setup and enable the timer interrupt */
	err = bus_setup_intr(dev, sc->res[1], INTR_TYPE_CLK, imx_gpt_intr,
	    NULL, sc, &sc->sc_ih);
	if (err != 0) {
		bus_release_resources(dev, imx_gpt_spec, sc->res);
		device_printf(dev, "Unable to setup the clock irq handler, "
		    "err = %d\n", err);
		return (ENXIO);
	}

	sc->et.et_name = "i.MXxxx GPT Eventtimer";
	sc->et.et_flags = ET_FLAGS_ONESHOT;
	sc->et.et_quality = 1000;
	sc->et.et_frequency = sc->clkfreq;
	sc->et.et_min_period.sec = 0;
	sc->et.et_min_period.frac =
	    ((0x00000002LLU << 32) / sc->et.et_frequency) << 32;
	sc->et.et_max_period.sec = 0xfffffff0U / sc->et.et_frequency;
	sc->et.et_max_period.frac =
	    ((0xfffffffeLLU << 32) / sc->et.et_frequency) << 32;
	sc->et.et_start = imx_gpt_timer_start;
	sc->et.et_stop = imx_gpt_timer_stop;
	sc->et.et_priv = sc;
	et_register(&sc->et);

	/* Disable interrupts */
	WRITE4(sc, IMX_GPT_IR, 0);
	/* ACK any panding interrupts */
	WRITE4(sc, IMX_GPT_SR, (GPT_IR_ROV << 1) - 1);

	if (device_get_unit(dev) == 0)
	    imx_gpt_sc = sc;

	imx_gpt_timecounter.tc_frequency = sc->clkfreq;
	tc_init(&imx_gpt_timecounter);

	printf("clock: hz=%d stathz = %d\n", hz, stathz);

	device_printf(sc->sc_dev, "timer clock frequency %d\n", sc->clkfreq);

	imx_gpt_delay_count = imx51_get_clock(IMX51CLK_ARM_ROOT) / 4000000;

	SET4(sc, IMX_GPT_CR, GPT_CR_EN);

	return (0);
}

static int
imx_gpt_timer_start(struct eventtimer *et, struct bintime *first,
    struct bintime *period)
{
	struct imx_gpt_softc *sc;
	uint32_t ticks;

	sc = (struct imx_gpt_softc *)et->et_priv;
	if (first != NULL) {

		ticks = (et->et_frequency * (first->frac >> 32)) >> 32;
		if (first->sec != 0)
			ticks += et->et_frequency * first->sec;

		/*
		 * TODO: setupt second compare reg with time which will save
		 * us in case correct one lost, f.e. if period to short and
		 * setup done later than counter reach target value.
		 */
		/* Do not disturb, otherwise event will be lost */
		spinlock_enter();
		/* Set expected value */
		WRITE4(sc, IMX_GPT_OCR1, READ4(sc, IMX_GPT_CNT) + ticks);
		/* Enable compare register 1 Interrupt */
		SET4(sc, IMX_GPT_IR, GPT_IR_OF1);
		/* Now everybody can relax */
		spinlock_exit();

		return (0);
	}

	return (EINVAL);
}

static int
imx_gpt_timer_stop(struct eventtimer *et)
{
	struct imx_gpt_softc *sc;

	sc = (struct imx_gpt_softc *)et->et_priv;

	/* Disable OF1 Interrupt */
	CLEAR4(sc, IMX_GPT_IR, GPT_IR_OF1);
	WRITE4(sc, IMX_GPT_SR, GPT_IR_OF1);

	return (0);
}

int
imx_gpt_get_timerfreq(struct imx_gpt_softc *sc)
{

	return (sc->clkfreq);
}

void
cpu_initclocks(void)
{

	if (!imx_gpt_sc) {
		panic("%s: driver has not been initialized!", __func__);
	}

	cpu_initclocks_bsp();

	/* Switch to DELAY using counter */
	imx_gpt_delay_count = 0;
	device_printf(imx_gpt_sc->sc_dev,
	    "switch DELAY to use H/W counter\n");
}

static int
imx_gpt_intr(void *arg)
{
	struct imx_gpt_softc *sc;
	uint32_t status;

	sc = (struct imx_gpt_softc *)arg;

	/* Sometime we not get staus bit when interrupt arrive.  Cache? */
	while (!(status = READ4(sc, IMX_GPT_SR)))
		;

	if (status & GPT_IR_OF1) {
		if (sc->et.et_active) {
			sc->et.et_event_cb(&sc->et, sc->et.et_arg);
		}
	}

	/* ACK */
	WRITE4(sc, IMX_GPT_SR, status);

	return (FILTER_HANDLED);
}

u_int
imx_gpt_get_timecount(struct timecounter *tc)
{

	if (imx_gpt_sc == NULL)
		return (0);

	return (READ4(imx_gpt_sc, IMX_GPT_CNT));
}

static device_method_t imx_gpt_methods[] = {
	DEVMETHOD(device_probe,		imx_gpt_probe),
	DEVMETHOD(device_attach,	imx_gpt_attach),

	DEVMETHOD_END
};

static driver_t imx_gpt_driver = {
	"imx_gpt",
	imx_gpt_methods,
	sizeof(struct imx_gpt_softc),
};

static devclass_t imx_gpt_devclass;

DRIVER_MODULE(imx_gpt, simplebus, imx_gpt_driver, imx_gpt_devclass, 0, 0);

void
DELAY(int usec)
{
	int32_t counts;
	uint32_t last;

	/*
	 * Check the timers are setup, if not just use a for loop for the
	 * meantime.
	 */
	if (imx_gpt_delay_count) {
		for (; usec > 0; usec--)
			for (counts = imx_gpt_delay_count; counts > 0;
			    counts--)
				/* Prevent optimizing out the loop */
				cpufunc_nullop();
		return;
	}

	/* At least 1 count */
	usec = MAX(1, usec / 100);

	last = READ4(imx_gpt_sc, IMX_GPT_CNT) + usec;
	while (READ4(imx_gpt_sc, IMX_GPT_CNT) < last) {
		/* Prevent optimizing out the loop */
		cpufunc_nullop();
	}
	/* TODO: use interrupt on OCR2 */
}