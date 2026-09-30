public class LedgerCaller {
    public async Task Snapshot(LedgerService.LedgerServiceClient client, ChargeRequest request) {
        await client.SnapshotAsync(request);
    }
}
