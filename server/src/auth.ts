import type { FastifyReply, FastifyRequest, HookHandlerDoneFunction } from "fastify";

import { HttpError } from "./errors.js";

interface PublicRouteConfig {
  public?: boolean;
}

export function createAuthHook(apiToken: string) {
  return async function authenticate(request: FastifyRequest, _reply: FastifyReply): Promise<void> {
    const routeConfig = request.routeOptions.config as PublicRouteConfig | undefined;
    if (routeConfig?.public === true) {
      return;
    }

    const authorizationHeader = request.headers.authorization;
    if (!authorizationHeader) {
      throw new HttpError(401, "Missing authorization token");
    }

    const [scheme, token, ...remainingParts] = authorizationHeader.split(" ");
    if (
      scheme !== "Bearer" ||
      token === undefined ||
      token.length === 0 ||
      remainingParts.length > 0 ||
      token !== apiToken
    ) {
      throw new HttpError(403, "Invalid API token");
    }
  };
}

export type AuthHook = (
  request: FastifyRequest,
  reply: FastifyReply,
  done: HookHandlerDoneFunction,
) => void | Promise<void>;
