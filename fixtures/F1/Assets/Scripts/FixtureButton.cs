using UnityEngine;
using UnityEngine.UI;

public class FixtureButton : MonoBehaviour
{
    void Start() => GetComponent<Button>().onClick.AddListener(FixturePlayer.Click);
}
