# Security notes (before publishing)

## Do not commit secrets

- Device shared secrets (`DEVICE_SECRET`, `devices.json`)
- Recipient emails (`alert_recipients.json`)
- Server hostnames / IPs if you consider them sensitive

This template repo avoids including those files. Create them locally from the `*.example.json` files.

## If you accidentally published a secret

1. **Rotate** the secret immediately (create a new one).
2. Remove the secret from the repository.
3. Rewrite git history (optional, but recommended):
   - `git filter-repo` or BFG Repo-Cleaner
4. Force-push, and assume anyone may already have copied the old secret.
