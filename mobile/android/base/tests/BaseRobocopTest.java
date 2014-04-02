/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.gecko.tests;

import java.util.Map;

import org.mozilla.gecko.Assert;
import org.mozilla.gecko.FennecInstrumentationTestRunner;
import org.mozilla.gecko.FennecMochitestAssert;
import org.mozilla.gecko.FennecNativeDriver;
import org.mozilla.gecko.FennecTalosAssert;

import android.app.Activity;
import android.test.ActivityInstrumentationTestCase2;

public abstract class BaseRobocopTest extends ActivityInstrumentationTestCase2<Activity> {
    public enum Type {
        MOCHITEST,
        TALOS
    }

    protected static final String TARGET_PACKAGE_ID = "org.mozilla.gecko";
    protected Assert mAsserter;
    protected String mLogFile;

    protected Map<?, ?> mConfig;
    protected String mRootPath;

    public BaseRobocopTest(Class<Activity> activityClass) {
        super(activityClass);
    }

    @SuppressWarnings("deprecation")
    public BaseRobocopTest(String targetPackageId, Class<Activity> activityClass) {
        super(targetPackageId, activityClass);
    }

    /**
     * Returns the test type: mochitest or talos.
     * <p>
     * By default tests are mochitests, but a test can override this method in
     * order to change its type. Most Robocop tests are mochitests.
     */
    protected Type getTestType() {
        return Type.MOCHITEST;
    }

    @Override
    protected void setUp() throws Exception {
        // Load config file from root path (set up by Python script).
        mRootPath = FennecInstrumentationTestRunner.getFennecArguments().getString("deviceroot");
        String configFile = FennecNativeDriver.getFile(mRootPath + "/robotium.config");
        mConfig = FennecNativeDriver.convertTextToTable(configFile);
        mLogFile = (String) mConfig.get("logfile");

        // Initialize the asserter.
        if (getTestType() == Type.TALOS) {
            mAsserter = new FennecTalosAssert();
        } else {
            mAsserter = new FennecMochitestAssert();
        }
        mAsserter.setLogFile(mLogFile);
        mAsserter.setTestName(this.getClass().getName());
    }
}