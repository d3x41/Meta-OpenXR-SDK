/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * Licensed under the Oculus SDK License Agreement (the "License");
 * you may not use the Oculus SDK except in compliance with the License,
 * which is provided at the time of installation or download, or which
 * otherwise accompanies this software in either electronic or hard copy form.
 *
 * You may obtain a copy of the License at
 * https://developer.oculus.com/licenses/oculussdk/
 *
 * Unless required by applicable law or agreed to in writing, the Oculus SDK
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package com.oculus;

import android.os.Bundle;
import android.util.Log;

public class MainNativeActivity extends android.app.NativeActivity {

  @Override
  public void onCreate(Bundle savedInstanceState) {
    Log.d(MainActivity.TAG, "MainNativeActivity.onCreate() called");
    super.onCreate(savedInstanceState);
  }

  // Called from native code to safely quit app.
  public void onNativeFinish() {
    Log.d(MainActivity.TAG, "MainNativeActivity finish called from native app.");
    finishAndRemoveTask();
  }

  @Override
  public void onDestroy() {
    Log.d(MainActivity.TAG, "MainNativeActivity.onDestroy() called");
    super.onDestroy();
  }
}
