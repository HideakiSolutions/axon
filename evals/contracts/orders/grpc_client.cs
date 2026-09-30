using System.Threading.Tasks;
public class BillingCaller {
    public async Task Charge(BillingService.BillingServiceClient client, ChargeRequest request) {
        await client.ChargeAsync(request);
    }
    public async Task Refund(BillingService.BillingServiceClient client, ChargeRequest request) {
        await client.RefundAsync(request);
    }
    public async Task Capture(BillingService.BillingServiceClient client, ChargeRequest request) {
        await client.CaptureAsync(request);
    }
    public async Task Cancel(BillingService.BillingServiceClient client, ChargeRequest request) {
        await client.CancelAsync(request);
    }
}
