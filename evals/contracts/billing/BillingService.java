public class BillingService {
    public ChargeReply charge(ChargeRequest request) {
        return ChargeReply.newBuilder().setAccepted(true).build();
    }
    public ChargeReply refund(ChargeRequest request) { return ChargeReply.getDefaultInstance(); }
    public ChargeReply capture(ChargeRequest request) { return ChargeReply.getDefaultInstance(); }
    public ChargeReply cancel(ChargeRequest request) { return ChargeReply.getDefaultInstance(); }
}
