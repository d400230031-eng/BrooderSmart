import fs from "fs";
import path from "path";

export const config = {
  api: {
    bodyParser: false
  }
};

export default async function handler(req, res) {
  res.setHeader("Access-Control-Allow-Origin", "*");
  res.setHeader("Access-Control-Allow-Methods", "POST, OPTIONS");
  res.setHeader("Access-Control-Allow-Headers", "Content-Type");

  if (req.method === "OPTIONS") {
    return res.status(200).end();
  }

  if (req.method !== "POST") {
    return res.status(405).json({ error: "Method not allowed" });
  }

  try {
    const chunks = [];
    for await (const chunk of req) {
      chunks.push(Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk));
    }
    const buffer = Buffer.concat(chunks);
    if (!buffer.length) {
      return res.status(400).json({ error: "Empty image payload" });
    }

    const tmpFile = path.join("/tmp", "latest_jalak.jpg");
    fs.writeFileSync(tmpFile, buffer);

    const ts = Date.now();
    const host = req.headers["x-forwarded-host"] || req.headers.host || "smartbrooder-three.vercel.app";
    const proto = req.headers["x-forwarded-proto"] || "https";
    const photoUrl = `${proto}://${host}/api/photo?t=${ts}`;

    return res.status(200).json({
      ok: true,
      size: buffer.length,
      url: photoUrl
    });
  } catch (err) {
    return res.status(500).json({ error: err.message || "Upload failed" });
  }
}