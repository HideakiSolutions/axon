using System.Threading.Tasks;

public class ShadowedCaller {
    public async Task Billing(BillingService.BillingServiceClient client,
                              ChargeRequest request) {
        await Task.CompletedTask;
    }

    public async Task ChargeOther(OtherClient client, ChargeRequest request) {
        await client.ChargeAsync(request);
    }
}
