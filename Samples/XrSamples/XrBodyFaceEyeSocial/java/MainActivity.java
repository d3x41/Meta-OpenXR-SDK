// Copyright (c) Facebook Technologies, LLC and its affiliates. All Rights reserved.
package com.oculus.xrsamples.xrbodyfaceeyesocial;

import android.content.pm.PackageManager;
import android.os.Bundle;
import java.util.ArrayList;
import java.util.List;

public class MainActivity extends android.app.NativeActivity {
  private static final String PERMISSION_EYE_TRACKING = "com.oculus.permission.EYE_TRACKING";
  private static final String PERMISSION_FACE_TRACKING = "com.oculus.permission.FACE_TRACKING";
  private static final String PERMISSION_RECORD_AUDIO = "android.permission.RECORD_AUDIO";
  private static final int REQUEST_CODE_PERMISSION_EYE_AND_FACE_TRACKING = 1;

  @Override
  protected void onCreate(Bundle savedInstanceState) {
    super.onCreate(savedInstanceState);

    // This sample makes use of eye and face tracking data at all times
    // so we are requesting it in onCreate. If an app, only requires eye
    // and face tracking data deep in their experience it may make sense
    // to query this later in the app lifecycle.
    requestFaceEyeTrackingPermissionsIfNeeded();
  }

  private void requestFaceEyeTrackingPermissionsIfNeeded() {
    List<String> permissionsToRequest = new ArrayList<>();
    if (checkSelfPermission(PERMISSION_EYE_TRACKING) != PackageManager.PERMISSION_GRANTED) {
      permissionsToRequest.add(PERMISSION_EYE_TRACKING);
    }
    if (checkSelfPermission(PERMISSION_FACE_TRACKING) != PackageManager.PERMISSION_GRANTED) {
      permissionsToRequest.add(PERMISSION_FACE_TRACKING);
    }
    if (checkSelfPermission(PERMISSION_RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
      permissionsToRequest.add(PERMISSION_RECORD_AUDIO);
    }

    if (!permissionsToRequest.isEmpty()) {
      String[] permissionsAsArray =
          permissionsToRequest.toArray(new String[permissionsToRequest.size()]);
      requestPermissions(permissionsAsArray, REQUEST_CODE_PERMISSION_EYE_AND_FACE_TRACKING);
    }
  }
}
