export default function handler(req, res) {
  res.setHeader("Access-Control-Allow-Origin", "*");
  if (req.method !== "GET") {
    return res.status(405).json({ error: "Method not allowed" });
  }

  const latest = globalThis.__latestJalakPhoto;
  if (!latest || !latest.buffer || !latest.buffer.length) {
    return res.status(404).json({ error: "Belum ada foto di memori server" });
  }

  res.setHeader("Content-Type", "image/jpeg");
  res.setHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  res.setHeader("X-Photo-Timestamp", String(latest.ts));
  return res.status(200).send(latest.buffer);
}