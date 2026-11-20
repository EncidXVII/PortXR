// Copyright (C) 2026 EncidXVII
// SPDX-License-Identifier: GPL-3.0-only

using UnityEngine;
using UnityEngine.UI;

public class FixtureButton : MonoBehaviour
{
    void Start() { GetComponent<Button>().onClick.AddListener(FixturePlayer.Click); }
}
