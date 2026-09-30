public class ShippingCaller {
    public async Task Charge(ShippingService.ShippingServiceClient client, ChargeRequest request) {
        await client.ChargeAsync(request);
    }
}
