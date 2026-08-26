/* SPDX-License-Identifier: Apache-2.0
 * Licensed under the Apache License, Version 2.0.
 */

declare module "@nyabula/core" {
  export interface PluginInfo {
    id: string;
    version: string;
    apiVersion: number;
  }

  export function info(): PluginInfo;
  export function log(...values: unknown[]): void;
}

declare module "@nyabula/storage" {
  export function get(key: string): string | null;
  export function put(key: string, value: string): void;
}

declare module "@nyabula/network" {
  export function request(request: unknown): Promise<unknown>;
}

declare module "@nyabula/ui" {
  export function notify(message: string): Promise<void>;
}

declare module "@nyabula/ai" {
  export function invoke(prompt: unknown): Promise<unknown>;
}
