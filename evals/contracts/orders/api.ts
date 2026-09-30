import express from 'express';
const app = express();
app.post('/v1/orders', function createOrder(req, res) {
  res.json({id: req.body.id, status: 'created'});
});
app.get('/v1/orders/list', function listOrders(req, res) { res.json([]); });
app.post('/v1/orders/confirm', function confirmOrder(req, res) { res.json({ok: true}); });
app.put('/v1/orders/status', function updateStatus(req, res) { res.json({ok: true}); });
app.delete('/v1/orders/archive', function archiveOrder(req, res) { res.json({ok: true}); });
export default app;
