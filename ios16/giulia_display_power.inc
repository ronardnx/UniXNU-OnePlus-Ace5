/* Included after AppleCLCD. Only iOS display-power requests call this path. */
extern "C" vm_offset_t ml_io_map(vm_offset_t,vm_size_t);
static uint32_t giulia_panel_read(void *context,unsigned offset)
{
    vm_offset_t base=(vm_offset_t)context;
    uint32_t value=*(volatile uint32_t *)(base+offset);
    __asm__ volatile("dsb sy" ::: "memory");return value;
}
static void giulia_panel_write(void *context,unsigned offset,uint32_t value)
{
    vm_offset_t base=(vm_offset_t)context;
    *(volatile uint32_t *)(base+offset)=value;
    __asm__ volatile("dsb sy" ::: "memory");
}
static void giulia_panel_delay(void *context,unsigned us){(void)context;IODelay(us);}

IOReturn AppleCLCD::setDisplayPower(uint64_t requested)
{
    if(!fDisplayPowerLock || !fCaptureLock)return kIOReturnNotReady;
    IOLockLock(fDisplayPowerLock);
    IOLockLock(fCaptureLock);
    int error=GDP_IO;
    if(!fDisplayRegisters)fDisplayRegisters=ml_io_map(0x0ae94000,0x4000);
    if(fDisplayRegisters) {
        const struct gdp_io io={(void *)fDisplayRegisters,giulia_panel_read,
            giulia_panel_write,giulia_panel_delay};
        error=gdp_power(&io,&fDisplayPower,requested!=0);
        if(fDisplayPower.known)fDisplayPowerOff=!fDisplayPower.on;
    }
    IOLockUnlock(fCaptureLock);
    if(!error && requested) {
        IOLockLock(fLock);fDirty=true;IOLockUnlock(fLock);
    }
    setProperty("GiuliaDisplayPowerRequested",requested,64);
    setProperty("GiuliaDisplayPowerKnown",fDisplayPower.known!=0);
    setProperty("GiuliaDisplayPowerOn",fDisplayPower.on!=0);
    setProperty("GiuliaDisplayPowerError",error,32);
    setProperty("GiuliaDisplayPowerFrozen",fDisplayPower.frozen!=0);
    IOLog("GIULIA DISPLAY POWER: request=%llu known=%u on=%u error=%d frozen=%u irq=%#x status=%#x ack=%#x requests=%u transitions=%u\n",
        (unsigned long long)requested,fDisplayPower.known,fDisplayPower.on,error,
        fDisplayPower.frozen,fDisplayPower.interrupts,fDisplayPower.status,
        fDisplayPower.ack,fDisplayPower.requests,fDisplayPower.transitions);
    IOLockUnlock(fDisplayPowerLock);
    return error==GDP_OK?kIOReturnSuccess:error==GDP_BUSY?kIOReturnBusy:kIOReturnIOError;
}
