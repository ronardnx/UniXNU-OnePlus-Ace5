/* Appended after the actual service/broker sources by the unified runner. */
#define GIULIA_PLATFORM_BROKER_ENABLED 1
#include "giulia_gpu_ipcc_owner.h"
extern "C" void modem_service_scenario(void)
{
    static int modemToken,wifiToken,audioToken,gpuToken;
    assert(gpr_global_initialize()==GPR_OK);
    struct gpr_key modemChannel={GPR_IPCC,0,2,0,0x406000,0x1000};
    struct gpr_token channel={};struct ggi_owner owner={};
    assert(gpr_global_open_epoch(&modemToken,1)==GPR_OK);
    assert(gpr_global_claim(&modemChannel,&modemToken,1,0,0,&channel)==GPR_OK);
    assert(ggi_claim(&owner,GGI_HOST_QMP,&wifiToken));
    assert(ggi_claim(&owner,GGI_HOST_AUDIO,&audioToken));
    static giulia_wifi_runtime wifiRuntime={};static int wifiSession;
    struct gpr_key sharedQmpRoute={GPR_IPCC,0,0,0,0x406000,0x1000};
    struct gpr_token modemQmpRoute={},modemMailbox={};
    assert(gpr_global_claim(&sharedQmpRoute,&modemToken,1,GPR_IPCC_SHARED_ROUTE,0,&modemQmpRoute)==GPR_OK);
    assert(gpr_global_legacy_ipcc_claim(1,&wifiRuntime));
    assert(gqp_claim(&wifiRuntime,&wifiSession));
    assert(gqp_owns(&wifiRuntime,&wifiSession));
    struct gpr_key mailbox={GPR_QMP,0,0,0,0x0c300000,0x400};
    assert(gpr_global_claim(&mailbox,&modemToken,1,0,0,&modemMailbox)==GPR_CONFLICT);
    gqp_release(&wifiRuntime,&gpuToken);assert(gqp_owns(&wifiRuntime,&wifiSession));
    gqp_release(&wifiRuntime,&wifiSession);
    assert(!gqp_owns(&wifiRuntime,&wifiSession));
    assert(gpr_global_claim(&mailbox,&modemToken,1,0,0,&modemMailbox)==GPR_OK);
    assert(!gqp_claim(&wifiRuntime,&wifiSession));
    assert(gpr_global_release(&modemMailbox,&modemToken,1)==GPR_OK);
    assert(gqp_claim(&wifiRuntime,&wifiSession));gqp_release(&wifiRuntime,&wifiSession);
    modem_aop_init();struct gwq_pmu_param voltage={};
    strcpy(voltage.pin_name,"VDD095_MX_PMU");voltage.wake_valid=1;voltage.wake_mv=600;
    assert(gq_power(&wifiRuntime,&voltage,1)==QPT_OK && modemAop.consumed==1);
    assert(!gw_qmp.owner && !gw_qmp.lease && !gw_qmp.state.owned);
    assert(gpr_global_legacy_ipcc_owns(1,&wifiRuntime));
    assert(gpr_global_claim(&mailbox,&modemToken,1,0,0,&modemMailbox)==GPR_OK);
    assert(gq_power(&wifiRuntime,&voltage,1)==QPT_BUSY);
    assert(modemAop.consumed==1 && gpr_global_owns(&modemMailbox,&modemToken,1));
    assert(gpr_global_release(&modemMailbox,&modemToken,1)==GPR_OK);
    assert(gq_power(&wifiRuntime,&voltage,1)==QPT_OK && modemAop.consumed==2);
    gw_qmp_stop(&wifiRuntime);
    assert(!wifiRuntime.quarantine);
    assert(gpr_global_legacy_ipcc_release(1,&wifiRuntime));
    assert(gpr_global_release(&modemQmpRoute,&modemToken,1)==GPR_OK);
    assert(!ggi_claim(&owner,GGI_GPU,&gpuToken));
    assert(ggi_release(&owner,GGI_HOST_QMP,&wifiToken));
    assert(ggi_release(&owner,GGI_HOST_AUDIO,&audioToken));
    assert(!ggi_claim(&owner,GGI_GPU,&gpuToken));
    assert(gpr_global_release(&channel,&modemToken,1)==GPR_OK);
    assert(ggi_claim(&owner,GGI_GPU,&gpuToken));
    assert(gpr_global_claim(&modemChannel,&modemToken,1,0,0,&channel)==GPR_CONFLICT);
    assert(ggi_release(&owner,GGI_GPU,&gpuToken));

    /* A failed close retains the same production lease, blocking another
     * subsystem even though all voltage commands were already consumed. */
    assert(gpr_global_legacy_ipcc_claim(1,&wifiRuntime));modem_aop_init();modemAop.ack_close=0;
    assert(gq_power(&wifiRuntime,&voltage,1)==QPT_TIMEOUT);
    assert(wifiRuntime.qmp_retained && gw_qmp.owner==&wifiRuntime && gw_qmp.state.leased);
    assert(gpr_global_claim(&mailbox,&modemToken,1,0,0,&modemMailbox)==GPR_CONFLICT);
    gw_qmp_stop(&wifiRuntime);assert(gw_qmp.state.leased && gqp_owns(&wifiRuntime,&gw_qmp.state));

    struct gmp_platform invalid={};invalid.abi=GMP_ABI;invalid.bytes=sizeof(invalid);invalid.source=2;
    assert(!giulia_modem_platform_register(&invalid));
    GiuliaModem *service=new GiuliaModem;
    assert(service->init());assert(service->start(nullptr));
    struct gmr_snapshot before={};assert(giulia_modem_get_snapshot(&before)==GMR_OK);
    assert(before.lifecycle==GMR_OFF && !before.resources_owned);
    assert(giulia_modem_terminal("modem.start"));
    struct gmr_snapshot after={};
    for(unsigned i=0;i<100;i++) {
        assert(giulia_modem_get_snapshot(&after)==GMR_OK);
        if(after.last_error)break;
        IOSleep(2);
    }
    assert(after.last_error==GMR_UNSUPPORTED && !after.resources_owned && !after.firmware_running);
    assert(giulia_modem_terminal("modem.status"));service->stop(nullptr);
    assert(giulia_modem_get_snapshot(&after)==GMR_STATE);
    assert(giulia_modem_terminal("modem.start"));
    std::lock_guard<std::mutex> logGuard(modemLogLock);
    bool identity=false,epoch=false;
    for(const auto& line:modemLogs) {
        identity=identity || line.find("GIULIA MODEM: runtime ABI=1;")!=std::string::npos;
        epoch=epoch || line.find("GIULIA MODEM: epoch=")!=std::string::npos;
    }
    assert(identity && epoch);
}
