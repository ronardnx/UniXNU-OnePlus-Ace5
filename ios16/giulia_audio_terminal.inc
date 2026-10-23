/* Audio evidence queries. No MMIO mapping, DSP load or resource takeover. */
static int gaudio_command(const char *cmd)
{
    if(cmd && giulia_audio_runtime_command(cmd))return 1;
    if(!cmd || (strcmp(cmd,"audio.status") && strcmp(cmd,"audio.map")))return 0;
    printf("GIULIA AUDIO: stage=board-mapping-only hardware_ready=0 playback=0 recording=0\n");
    if(!strcmp(cmd,"audio.status"))return 1;
    printf("GIULIA AUDIO: dts=%s source=%s\n",GAMAP_DTS_SHA256,GAMAP_SOURCE_COMMIT);
    for(unsigned i=0;i<GAMAP_REGIONS;i++) {
        const struct gamap_region *r=&gamap_regions[i];
        printf("GIULIA AUDIO: resource=%s pa=%#llx bytes=%#llx kind=%s android_enabled=%u\n",
            r->name,(unsigned long long)r->physical,(unsigned long long)r->bytes,
            r->kind==GAMAP_MMIO?"mmio":"reserved-dsp-ram",r->enabled);
    }
    for(unsigned i=0;i<sizeof(gamap_irqs)/sizeof(gamap_irqs[0]);i++)
        printf("GIULIA AUDIO: irq=%s spi=%u intid=%u\n",
            gamap_irqs[i].name,gamap_irqs[i].spi,gamap_irqs[i].intid);
    for(unsigned i=0;i<sizeof(gamap_devices)/sizeof(gamap_devices[0]);i++) {
        const struct gamap_bus_device *d=&gamap_devices[i];
        if(d->has_reset_gpio)
            printf("GIULIA AUDIO: device=%s bus=%s bus_address=%#llx reset_gpio=%u\n",
                d->name,d->bus,(unsigned long long)d->address,d->reset_gpio);
        else
            printf("GIULIA AUDIO: device=%s bus=%s bus_address=%#llx reset_gpio=unspecified\n",
                d->name,d->bus,(unsigned long long)d->address);
    }
    printf("GIULIA AUDIO: glink_pid=%u ipcc_client=%u signal=%u gpr_domain=%u channel=%s spf=%u packet=%u prm=%u\n",
        GAMAP_GLINK_PID,GAMAP_IPCC_CLIENT,GAMAP_IPCC_SIGNAL,GAMAP_GPR_DOMAIN,
        GAMAP_GPR_CHANNEL,GAMAP_SPF_SERVICE,GAMAP_PACKET_SERVICE,GAMAP_PRM_SERVICE);
    printf("GIULIA AUDIO: android_iova=%#llx bytes=%#llx xnu_dma_owned=0\n",
        GAMAP_IOVA_BASE,GAMAP_IOVA_BYTES);
    for(unsigned i=0;i<sizeof(gamap_sids)/sizeof(gamap_sids[0]);i++)
        printf("GIULIA AUDIO: audio_sid=%#x mask=%#x\n",gamap_sids[i].id,gamap_sids[i].mask);
    printf("GIULIA AUDIO: speakers=SIA9177-left+right interface=secondary-MI2S codec=WCD939x wsa884x=disabled\n");
    return 1;
}
