// Labeled pilot corpus. Each handler is deliberately independent.
export function saveName(req: any, db: any) {
  const name = req.body.name;
  const clean = name.trim();
  db.save(clean);
}

export function postEmail(request: any) {
  const email = request.body.email;
  fetch("https://example.invalid", { method: "POST", body: email });
}

export function saveConstant(db: any) {
  db.save("constant");
}

export function unrelatedRequest(req: any, db: any) {
  const ignored = req.body.name;
  db.save("constant");
}

export function saveAfterBranch(req: any, db: any) {
  let value = "";
  if (req.body.enabled) {
    value = req.body.name;
  }
  db.save(value);
}

export function saveInsideBranch(req: any, db: any, enabled: boolean) {
  if (enabled) {
    const value = req.body.name;
    db.save(value);
  }
}

export function saveInsideElse(req: any, db: any, enabled: boolean) {
  if (enabled) {
    return;
  } else {
    const value = req.body.name;
    db.save(value);
  }
}

export function overwrittenBothBranches(req: any, db: any, enabled: boolean) {
  let value = req.body.name;
  if (enabled) {
    value = "one";
  } else {
    value = "two";
  }
  db.save(value);
}

export function saveDynamic(req: any, db: any, key: string) {
  const value = req.body[key];
  db.save(value);
}

export function saveHelper(req: any, db: any) {
  const value = normalize(req.body.name);
  db.save(value);
}

function normalize(value: string) { return value.trim(); }
