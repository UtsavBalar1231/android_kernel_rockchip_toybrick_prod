CONFIG_WFX_SECURE_LINK ?= y
CFLAGS_debug.o = -I$(src)

wfx-y := \
	bh.o \
	hwio.o \
	fwio.o \
	hif_tx_mib.o \
	hif_tx.o \
	hif_rx.o \
	queue.o \
	data_tx.o \
	data_rx.o \
	scan.o \
	sta.o \
	key.o \
	main.o \
	nl80211_vendor.o \
	debug.o
wfx-$(CONFIG_SPI) += bus_spi.o
wfx-$(subst m,y,$(CONFIG_MMC)) += bus_sdio.o
wfx-$(CONFIG_WFX_SECURE_LINK) += \
	secure_link.o \
	mbedtls/library/aes.o \
	mbedtls/library/bignum.o \
	mbedtls/library/ccm.o \
	mbedtls/library/cipher.o \
	mbedtls/library/cipher_wrap.o \
	mbedtls/library/ecdh.o \
	mbedtls/library/ecp_curves.o \
	mbedtls/library/ecp.o \
	mbedtls/library/error.o \
	mbedtls/library/md.o \
	mbedtls/library/md_wrap.o \
	mbedtls/library/platform_util.o \
	mbedtls/library/sha256.o \
	mbedtls/library/sha512.o

ccflags-$(CONFIG_WFX_SECURE_LINK) += \
	-I$(src)/mbedtls/include -DCONFIG_WFX_SECURE_LINK=y

obj-$(CONFIG_WFX) += wfx.o
