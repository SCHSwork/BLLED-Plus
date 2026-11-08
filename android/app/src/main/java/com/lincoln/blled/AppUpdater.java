package com.lincoln.blled;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.DownloadManager;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.net.Uri;
import android.os.Build;
import android.os.Environment;
import android.widget.Toast;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.net.HttpURLConnection;
import java.net.URL;

/**
 * Keeps the app itself up to date: looks for a newer "app-v..." release of BLLED Plus on GitHub
 * (at most every 12 hours), offers it, downloads the APK and hands it to Android's installer.
 * Updates install over this version because every release is signed with the same key.
 */
class AppUpdater {
    private static final String RELEASES = "https://api.github.com/repos/SCHSwork/BLLED-Plus/releases?per_page=30";
    private static final long CHECK_EVERY_MS = 12L * 60 * 60 * 1000;

    private final Activity activity;
    private final SharedPreferences prefs;
    private long downloadId = -1;

    AppUpdater(Activity activity, SharedPreferences prefs) {
        this.activity = activity;
        this.prefs = prefs;
    }

    void checkSoon() {
        long last = prefs.getLong("appUpdateChecked", 0);
        if (System.currentTimeMillis() - last < CHECK_EVERY_MS) return;
        new Thread(this::check).start();
    }

    private String installedVersion() {
        try {
            return activity.getPackageManager().getPackageInfo(activity.getPackageName(), 0).versionName;
        } catch (Exception e) {
            return "0";
        }
    }

    /** "1.10" is newer than "1.9" */
    static boolean newer(String a, String b) {
        String[] x = a.split("\\."), y = b.split("\\.");
        for (int i = 0; i < Math.max(x.length, y.length); i++) {
            int p = i < x.length ? num(x[i]) : 0, q = i < y.length ? num(y[i]) : 0;
            if (p != q) return p > q;
        }
        return false;
    }

    private static int num(String s) {
        try {
            return Integer.parseInt(s.replaceAll("[^0-9]", ""));
        } catch (Exception e) {
            return 0;
        }
    }

    private void check() {
        try {
            HttpURLConnection c = (HttpURLConnection) new URL(RELEASES).openConnection();
            c.setRequestProperty("Accept", "application/vnd.github+json");
            c.setRequestProperty("User-Agent", "BLLED-Plus-app");
            c.setConnectTimeout(10000);
            c.setReadTimeout(15000);
            if (c.getResponseCode() != 200) return;
            StringBuilder sb = new StringBuilder();
            try (BufferedReader r = new BufferedReader(new InputStreamReader(c.getInputStream(), "UTF-8"))) {
                String line;
                while ((line = r.readLine()) != null) sb.append(line);
            }
            prefs.edit().putLong("appUpdateChecked", System.currentTimeMillis()).apply();

            JSONArray list = new JSONArray(sb.toString());
            String bestVer = null, bestUrl = null;
            for (int i = 0; i < list.length(); i++) {
                JSONObject rel = list.getJSONObject(i);
                String tag = rel.optString("tag_name", "");
                if (!tag.startsWith("app-v") || rel.optBoolean("draft")) continue;
                String ver = tag.substring(5);
                JSONArray assets = rel.optJSONArray("assets");
                if (assets == null) continue;
                for (int j = 0; j < assets.length(); j++) {
                    JSONObject a = assets.getJSONObject(j);
                    if (a.optString("name").endsWith(".apk") && (bestVer == null || newer(ver, bestVer))) {
                        bestVer = ver;
                        bestUrl = a.optString("browser_download_url");
                    }
                }
            }
            String mine = installedVersion();
            if (bestVer != null && newer(bestVer, mine)) {
                final String v = bestVer, url = bestUrl;
                activity.runOnUiThread(() -> offer(v, mine, url));
            }
        } catch (Exception ignored) {
            // No internet or GitHub unreachable: try again next time
        }
    }

    private void offer(String ver, String mine, String url) {
        if (activity.isFinishing()) return;
        new AlertDialog.Builder(activity)
                .setTitle("App update available")
                .setMessage("BLLED Plus app " + ver + " is available. You have " + mine + ".")
                .setPositiveButton("Update", (d, w) -> download(ver, url))
                .setNegativeButton("Not now", null)
                .show();
    }

    private void download(String ver, String url) {
        try {
            DownloadManager dm = (DownloadManager) activity.getSystemService(Context.DOWNLOAD_SERVICE);
            DownloadManager.Request req = new DownloadManager.Request(Uri.parse(url))
                    .setTitle("BLLED Plus app " + ver)
                    .setMimeType("application/vnd.android.package-archive")
                    .setNotificationVisibility(DownloadManager.Request.VISIBILITY_VISIBLE_NOTIFY_COMPLETED)
                    .setDestinationInExternalPublicDir(Environment.DIRECTORY_DOWNLOADS, "BLLED-Plus-app-" + ver + ".apk");
            BroadcastReceiver done = new BroadcastReceiver() {
                @Override
                public void onReceive(Context ctx, Intent intent) {
                    if (intent.getLongExtra(DownloadManager.EXTRA_DOWNLOAD_ID, -2) != downloadId) return;
                    try {
                        activity.unregisterReceiver(this);
                    } catch (Exception ignored) {
                    }
                    install(dm);
                }
            };
            IntentFilter f = new IntentFilter(DownloadManager.ACTION_DOWNLOAD_COMPLETE);
            if (Build.VERSION.SDK_INT >= 33) {
                activity.registerReceiver(done, f, Context.RECEIVER_EXPORTED);
            } else {
                activity.registerReceiver(done, f);
            }
            downloadId = dm.enqueue(req);
            Toast.makeText(activity, "Downloading the update...", Toast.LENGTH_SHORT).show();
        } catch (Exception e) {
            // Fall back to the browser
            activity.startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(url)));
        }
    }

    private void install(DownloadManager dm) {
        Uri apk = dm.getUriForDownloadedFile(downloadId);
        if (apk == null) {
            Toast.makeText(activity, "The update didn't download. Try again later.", Toast.LENGTH_LONG).show();
            return;
        }
        // Android asks once to allow installs from this app, then shows its own install screen
        Intent i = new Intent(Intent.ACTION_VIEW);
        i.setDataAndType(apk, "application/vnd.android.package-archive");
        i.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_ACTIVITY_NEW_TASK);
        try {
            activity.startActivity(i);
        } catch (Exception e) {
            Toast.makeText(activity, "Open the downloaded BLLED Plus app file to install it.", Toast.LENGTH_LONG).show();
        }
    }
}
