import * as cp from "child_process";
import * as vscode from "vscode";
import {
  LanguageClient,
  LanguageClientOptions,
  ServerOptions,
  Trace
} from "vscode-languageclient/node";

let client: LanguageClient | undefined;
let webProcess: cp.ChildProcess | undefined;

function config() {
  return vscode.workspace.getConfiguration("axon");
}

function workspaceRoot(): string | undefined {
  const configured = config().get<string>("projectRoot", "");
  if (configured && configured.trim().length > 0) {
    return configured;
  }
  return vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
}

function axonPath(): string {
  return config().get<string>("path", "axon");
}

async function startLanguageServer(context: vscode.ExtensionContext) {
  const root = workspaceRoot();
  if (!root) {
    return;
  }

  const extraArgs = config().get<string[]>("lspArgs", []);
  const serverOptions: ServerOptions = {
    command: axonPath(),
    args: ["lsp", ...extraArgs],
    options: { cwd: root }
  };

  const clientOptions: LanguageClientOptions = {
    documentSelector: [
      { scheme: "file" }
    ],
    synchronize: {
      fileEvents: vscode.workspace.createFileSystemWatcher("**/*")
    },
    outputChannelName: "Axon Language Server"
  };

  client = new LanguageClient("axon", "Axon Language Server", serverOptions, clientOptions);
  const trace = config().get<string>("trace.server", "off");
  client.setTrace(trace === "verbose" ? Trace.Verbose : trace === "messages" ? Trace.Messages : Trace.Off);
  context.subscriptions.push(client);
  await client.start();
}

async function restartLanguageServer(context: vscode.ExtensionContext) {
  if (client) {
    await client.stop();
    client = undefined;
  }
  await startLanguageServer(context);
}

function runAxon(args: string[], cwd: string): Promise<void> {
  return new Promise((resolve, reject) => {
    const child = cp.spawn(axonPath(), args, { cwd, stdio: "pipe" });
    let stderr = "";
    child.stderr.on("data", chunk => {
      stderr += chunk.toString();
    });
    child.on("error", reject);
    child.on("exit", code => {
      if (code === 0) {
        resolve();
      } else {
        reject(new Error(stderr || `axon ${args.join(" ")} exited with code ${code}`));
      }
    });
  });
}

async function indexWorkspace() {
  const root = workspaceRoot();
  if (!root) {
    vscode.window.showErrorMessage("Axon requires an open workspace.");
    return;
  }
  await vscode.window.withProgress(
    { location: vscode.ProgressLocation.Notification, title: "Indexing workspace with Axon" },
    () => runAxon(["index", root], root)
  );
}

async function openWebExplorer() {
  const root = workspaceRoot();
  if (!root) {
    vscode.window.showErrorMessage("Axon requires an open workspace.");
    return;
  }
  const port = config().get<number>("webPort", 7070);
  if (!webProcess || webProcess.exitCode !== null) {
    webProcess = cp.spawn(axonPath(), ["web", `--port=${port}`], {
      cwd: root,
      stdio: "ignore",
      detached: false
    });
  }
  await vscode.env.openExternal(vscode.Uri.parse(`http://127.0.0.1:${port}`));
}

export async function activate(context: vscode.ExtensionContext) {
  context.subscriptions.push(
    vscode.commands.registerCommand("axon.restartLanguageServer", () => restartLanguageServer(context)),
    vscode.commands.registerCommand("axon.openWebExplorer", openWebExplorer),
    vscode.commands.registerCommand("axon.indexWorkspace", indexWorkspace)
  );
  await startLanguageServer(context);
}

export async function deactivate() {
  if (client) {
    await client.stop();
    client = undefined;
  }
  if (webProcess && webProcess.exitCode === null) {
    webProcess.kill();
  }
}
