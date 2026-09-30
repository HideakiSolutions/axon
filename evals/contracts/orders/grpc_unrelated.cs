using System.Threading.Tasks;

public class UnrelatedCaller {
    public async Task ChargeOther(BillingService.BillingServiceClient billing,
                                  OtherClient other, ChargeRequest request) {
        await other.ChargeAsync(request);
    }
}
