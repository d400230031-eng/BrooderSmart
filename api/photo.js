import fs from "fs";
import path from "path";

export default function handler(req, res) {
  res.setHeader("Access-Control-Allow-Origin", "*");
  if (req.method !== "GET") {
    return res.status(405).json({ error: "Method not allowed" });
  }

  const tmpFile = path.join("/tmp", "latest_jalak.jpg");
  if (!fs.existsSync(tmpFile)) {
    return res.status(404).json({ error: "Belum ada foto tersimpan" });
  }

  const img = fs.readFileSync(tmpFile);
  res.setHeader("Content-Type", "image/jpeg");
  res.setHeader("Cache-Control", "no-store, max-age=0");
  return res.status(200).send(img);
}