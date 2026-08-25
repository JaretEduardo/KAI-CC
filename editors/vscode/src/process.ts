// Shell-free child-process primitive (VS CODE COMPILER INTEGRATION
// MILESTONE 2 spec §12/§15): used to invoke both kaicc (compiler.ts) and
// the program it produces (commands.ts). Kept in its own vscode-API-free
// module (like paths.ts) so it stays directly unit-testable with plain
// Node - `vscode` is not resolvable outside a running extension host.

import { spawn } from 'child_process';

export interface ProcessResult {
    /** The child's exit code, or null if it never produced one (killed by signal, or never started). */
    exitCode: number | null;
    stdout: string;
    stderr: string;
    /** True if the process could not even be launched (e.g. ENOENT, not executable). */
    failedToStart: boolean;
    /** Set only when failedToStart is true. */
    startError?: string;
}

/**
 * Runs `command` with `args` as a plain child process - never a shell:
 * `child_process.spawn` is called with an argv array and `shell: false`,
 * so paths containing spaces are passed through exactly, with no
 * quoting/escaping concerns (never `exec("cmd " + input)`, never
 * `shell: true`, never `sh -c`/`bash -c`). Captures stdout/stderr/exit
 * code; never throws or rejects - a launch failure is reported through
 * the returned ProcessResult instead.
 */
export function spawnProcess(command: string, args: string[], cwd?: string): Promise<ProcessResult> {
    return new Promise((resolve) => {
        let settled = false;
        let stdout = '';
        let stderr = '';

        const child = spawn(command, args, { cwd, shell: false });

        child.stdout?.on('data', (chunk: Buffer) => {
            stdout += chunk.toString('utf8');
        });
        child.stderr?.on('data', (chunk: Buffer) => {
            stderr += chunk.toString('utf8');
        });

        child.on('error', (err: Error) => {
            if (settled) {
                return;
            }
            settled = true;
            resolve({ exitCode: null, stdout, stderr, failedToStart: true, startError: err.message });
        });

        child.on('close', (code: number | null) => {
            if (settled) {
                return;
            }
            settled = true;
            resolve({ exitCode: code, stdout, stderr, failedToStart: false });
        });
    });
}
