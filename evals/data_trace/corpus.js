export function saveQuery(req, repository) {
  const id = req.query.id;
  repository.create(id);
}

export function noFlow(req, repository) {
  const id = req.query.id;
  repository.create(123);
}
