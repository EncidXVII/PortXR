using System;
using System.IO;
using UnityEngine;
using UnityEngine.SceneManagement;

// Gamepad/mouse player that logs its state for the harness (FIXTURE_LOG env var).
public class FixturePlayer : MonoBehaviour
{
    public Transform head;
    CharacterController body;
    float yaw, pitch, vy;
    int jumps;
    static int clicks;
    static StreamWriter log;
    float nextLog;

    public static void Click() => clicks++;

    void Start()
    {
        body = GetComponent<CharacterController>();
        var path = Environment.GetEnvironmentVariable("FIXTURE_LOG");
        if (log == null && !string.IsNullOrEmpty(path)) log = new StreamWriter(path, true) { AutoFlush = true };
    }

    void Update()
    {
        yaw += Input.GetAxis("RightX") * 120f * Time.deltaTime + Input.GetAxis("Mouse X") * 2f;
        pitch = Mathf.Clamp(pitch - Input.GetAxis("Mouse Y") * 2f, -80f, 80f);
        transform.rotation = Quaternion.Euler(0, yaw, 0);
        head.localRotation = Quaternion.Euler(pitch, 0, 0);

        if (body.isGrounded)
        {
            vy = -1f;
            if (Input.GetKeyDown(KeyCode.JoystickButton0) || Input.GetKeyDown(KeyCode.Space))
            {
                vy = 5f;
                jumps++;
            }
        }
        else vy -= 9.81f * Time.deltaTime;
        var move = transform.TransformDirection(new Vector3(Input.GetAxis("Horizontal"), 0, Input.GetAxis("Vertical"))) * 3f;
        body.Move((move + Vector3.up * vy) * Time.deltaTime);

        if (Input.GetKeyDown(KeyCode.JoystickButton3) || Input.GetKeyDown(KeyCode.N))
            SceneManager.LoadScene(1 - SceneManager.GetActiveScene().buildIndex);

        if (log != null && Time.unscaledTime >= nextLog)
        {
            nextLog = Time.unscaledTime + 0.2f;
            var p = transform.position;
            log.WriteLine(FormattableString.Invariant(
                $"{{\"t\":{Time.unscaledTime:0.00},\"pos\":[{p.x:0.000},{p.y:0.000},{p.z:0.000}],\"yaw\":{yaw:0.0},\"jumps\":{jumps},\"clicks\":{clicks},\"scene\":{SceneManager.GetActiveScene().buildIndex}}}"));
        }
    }
}
