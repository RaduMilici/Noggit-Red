# Download game data through the R2 API

Noggit -> HTTPS Worker on workers.dev -> GAME_DATA R2 binding -> wow-extracted-data.
This bypasses the public r2.dev development endpoint. No purchased domain or
Cloudflare credentials in Noggit are needed. The endpoint intentionally publishes
files directly inside dbc/, maps/, vmaps/, and mmaps/; it offers no upload, delete,
or bucket listing. Only put redistributable data in these public paths.

## Deploy using the Cloudflare dashboard (no local tools needed)

1. Open Workers & Pages in your Cloudflare account and create a Worker named
   `noggit-game-data` using the Hello World template.
2. Open its code editor, replace the template with `worker.mjs`, and deploy.
3. In the Worker's Bindings tab, add an R2 bucket binding. Name it `GAME_DATA`
   and select your existing `wow-extracted-data` bucket. Deploy the binding change.
4. Copy the Worker URL shown by Cloudflare. It will look like
   `https://noggit-game-data.YOUR-SUBDOMAIN.workers.dev`.
5. Test `YOUR-WORKER-URL/dbc/AnimationData.dbc` in a browser. It should download
   the file. A 503 means the binding is missing or the storage request failed;
   a 404 means the requested object is absent or the path is not permitted.
6. Change ONLY `baseUrl` in `../game-data.json` to your actual Worker URL with
   a trailing slash. Preserve the file paths, sizes, and SHA-256 values.
7. Rebuild/package Noggit using the normal Windows or Ubuntu build instructions.
   The existing downloader already supports HTTPS endpoints, so no platform-specific
   Cloudflare SDK is needed. End users install no extra software.

Alternatively, with Node.js installed, run in this directory:

```bash
npx wrangler login
npx wrangler deploy
```

The bucket objects do not need to move or be uploaded again. Leave the old r2.dev
endpoint available for already published Noggit packages until they are replaced.
Changing the manifest currently changes Noggit's download-cache identity, so an
existing partial cache will not automatically carry over to the new manifest.

The Worker streams bodies without buffering the full file and supports GET, HEAD,
and single HTTP byte ranges. Range support is available for future resumable clients;
Noggit's current downloader still retries only when the user starts it again and
does not resume a partial file. Changing hosting alone does not fix that behavior.

Workers Free has a 100,000-request daily limit shared by Workers on the account.
With the current 12,054-file manifest, about eight full installations per day fit
before retries and other account traffic. Larger releases should use fewer bundled
objects or a plan with more requests. R2 storage/operation quotas also apply.

References:
- https://developers.cloudflare.com/r2/get-started/workers-api/
- https://developers.cloudflare.com/workers/configuration/routing/workers-dev/
- https://developers.cloudflare.com/workers/platform/limits/

Local handler checks: `node --test worker.test.mjs`. These use a fake R2 binding;
test the deployed URL separately before updating the release manifest.
