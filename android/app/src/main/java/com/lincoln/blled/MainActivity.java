package com.lincoln.blled;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.text.InputType;
import android.view.View;
import android.view.WindowInsets;
import android.webkit.HttpAuthHandler;
import android.webkit.JavascriptInterface;
import android.webkit.ValueCallback;
import android.webkit.WebChromeClient;
import android.webkit.WebResourceError;
import android.webkit.WebResourceRequest;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.EditText;
import android.widget.LinearLayout;

/**
 * BLLED companion app: shows the controller's own web interface full screen,
 * remembers its address, and offers a reconnect screen when it can't be reached.
 */
public class MainActivity extends Activity {

    private static final String PREFS = "blled";
    private static final String KEY_HOST = "host";
    private static final String DEFAULT_HOST = "blled.local";
    private static final String FALLBACK_HOST = "blled.local";
    private static final String CONNECT_PAGE = "file:///android_asset/connect.html";

    private static final int PICK_FILE = 1;

    private WebView web;
    private SharedPreferences prefs;
    private ValueCallback<Uri[]> pendingFilePick; // the page's file input waiting for a pick
    private boolean triedFallback = false;        // tried blled.local after the saved address failed

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        prefs = getSharedPreferences(PREFS, MODE_PRIVATE);

        getWindow().setStatusBarColor(Color.parseColor("#1f262e"));
        getWindow().setNavigationBarColor(Color.parseColor("#1f262e"));

        web = new WebView(this);
        web.setBackgroundColor(Color.parseColor("#1f262e"));
        // Keep the page clear of the status and navigation bars (Android 15 draws edge to edge)
        web.setOnApplyWindowInsetsListener((v, insets) -> {
            int top, bottom;
            if (android.os.Build.VERSION.SDK_INT >= 30) {
                android.graphics.Insets bars = insets.getInsets(WindowInsets.Type.systemBars());
                top = bars.top;
                bottom = bars.bottom;
            } else {
                top = insets.getSystemWindowInsetTop();
                bottom = insets.getSystemWindowInsetBottom();
            }
            v.setPadding(0, top, 0, bottom);
            return insets;
        });
        setContentView(web);

        WebSettings s = web.getSettings();
        s.setJavaScriptEnabled(true);
        s.setDomStorageEnabled(true);
        s.setSupportMultipleWindows(false); // links that open a new tab load in place
        s.setCacheMode(WebSettings.LOAD_DEFAULT);

        web.addJavascriptInterface(new Bridge(), "BLLEDApp");
        web.setWebViewClient(new Client());
        web.setWebChromeClient(new Chrome());

        new AppUpdater(this, prefs).checkSoon();

        if (savedInstanceState != null) {
            web.restoreState(savedInstanceState);
        } else if (prefs.contains(KEY_HOST)) {
            openController();
        } else {
            web.loadUrl(CONNECT_PAGE);
        }
    }

    private String host() {
        return prefs.getString(KEY_HOST, DEFAULT_HOST);
    }

    private void openController() {
        web.loadUrl("http://" + host() + "/status");
    }

    /** Called from connect.html */
    private class Bridge {
        @JavascriptInterface
        public String getHost() {
            return host();
        }

        @JavascriptInterface
        public void connect(String h) {
            triedFallback = false;
            String clean = h.trim().replaceFirst("^https?://", "").replaceAll("/.*$", "");
            if (clean.isEmpty()) return;
            prefs.edit().putString(KEY_HOST, clean).apply();
            runOnUiThread(MainActivity.this::openController);
        }
    }

    /** The controller sends the update page to its IP address; keep local addresses in the app. */
    private static boolean isLocal(String h) {
        if (h.endsWith(".local")) return true;
        if (h.startsWith("192.168.") || h.startsWith("10.")) return true;
        if (h.startsWith("172.")) {
            try {
                int b = Integer.parseInt(h.split("\\.")[1]);
                return b >= 16 && b <= 31;
            } catch (Exception e) {
                return false;
            }
        }
        return false;
    }

    private class Client extends WebViewClient {
        @Override
        public boolean shouldOverrideUrlLoading(WebView view, WebResourceRequest req) {
            Uri u = req.getUrl();
            // Stay inside the app for the controller; send anything else (wiki links etc.) to the browser
            if (u.getHost() != null && (u.getHost().equalsIgnoreCase(host()) || u.getHost().equalsIgnoreCase(FALLBACK_HOST) || isLocal(u.getHost()))) return false;
            if ("file".equals(u.getScheme())) return false;
            try {
                startActivity(new android.content.Intent(android.content.Intent.ACTION_VIEW, u));
            } catch (Exception ignored) {
            }
            return true;
        }

        @Override
        public void onReceivedError(WebView view, WebResourceRequest req, WebResourceError err) {
            if (!req.isForMainFrame()) return;
            // The router may have given the controller a new number: try its name before giving up
            if (!triedFallback && !FALLBACK_HOST.equalsIgnoreCase(host())) {
                triedFallback = true;
                view.loadUrl("http://" + FALLBACK_HOST + "/status");
                return;
            }
            view.loadUrl(CONNECT_PAGE + "?failed=1");
        }

        @Override
        public void onPageFinished(WebView view, String url) {
            Uri u = Uri.parse(url);
            // Reached it by name: remember that, so a changing number never matters again
            if (triedFallback && u.getHost() != null && u.getHost().equalsIgnoreCase(FALLBACK_HOST)) {
                prefs.edit().putString(KEY_HOST, FALLBACK_HOST).apply();
            }
        }

        @Override
        public void onReceivedHttpAuthRequest(WebView view, HttpAuthHandler handler, String h, String realm) {
            askLogin(handler);
        }
    }

    /** File uploads (firmware update, settings restore) and page dialogs. */
    private class Chrome extends WebChromeClient {
        @Override
        public boolean onShowFileChooser(WebView view, ValueCallback<Uri[]> callback, FileChooserParams params) {
            if (pendingFilePick != null) {
                pendingFilePick.onReceiveValue(null);
            }
            pendingFilePick = callback;
            // Any file type: Android doesn't know ".bin.ota", and filtering by it can grey the file out
            Intent pick = new Intent(Intent.ACTION_GET_CONTENT);
            pick.addCategory(Intent.CATEGORY_OPENABLE);
            pick.setType("*/*");
            try {
                startActivityForResult(Intent.createChooser(pick, "Choose a file"), PICK_FILE);
            } catch (Exception e) {
                pendingFilePick = null;
                return false;
            }
            return true;
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode == PICK_FILE && pendingFilePick != null) {
            pendingFilePick.onReceiveValue(WebChromeClient.FileChooserParams.parseResult(resultCode, data));
            pendingFilePick = null;
            return;
        }
        super.onActivityResult(requestCode, resultCode, data);
    }

    // The controller's optional WebUI login
    private void askLogin(HttpAuthHandler handler) {
        LinearLayout box = new LinearLayout(this);
        box.setOrientation(LinearLayout.VERTICAL);
        int pad = (int) (20 * getResources().getDisplayMetrics().density);
        box.setPadding(pad, pad / 2, pad, 0);

        EditText user = new EditText(this);
        user.setHint("Username");
        user.setText(prefs.getString("user", ""));
        EditText pass = new EditText(this);
        pass.setHint("Password");
        pass.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_PASSWORD);
        box.addView(user);
        box.addView(pass);

        new AlertDialog.Builder(this)
                .setTitle("Sign in to BLLED")
                .setView(box)
                .setCancelable(false)
                .setPositiveButton("Sign in", (d, w) -> {
                    prefs.edit().putString("user", user.getText().toString()).apply();
                    handler.proceed(user.getText().toString(), pass.getText().toString());
                })
                .setNegativeButton("Cancel", (d, w) -> {
                    handler.cancel();
                    web.loadUrl(CONNECT_PAGE);
                })
                .show();
    }

    @Override
    public void onBackPressed() {
        if (web.canGoBack()) {
            web.goBack();
        } else {
            super.onBackPressed();
        }
    }

    @Override
    protected void onSaveInstanceState(Bundle outState) {
        super.onSaveInstanceState(outState);
        web.saveState(outState);
    }

    @Override
    protected void onResume() {
        super.onResume();
        web.onResume();
    }

    @Override
    protected void onPause() {
        web.onPause();
        super.onPause();
    }
}
