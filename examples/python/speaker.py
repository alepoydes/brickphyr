from pybricks.hubs import PrimeHub
from pybricks.tools import multitask, run_task, wait

hub = PrimeHub()
hub.speaker.volume(10)


async def melody():
    await hub.speaker.beep(440, 80)
    await hub.speaker.play_notes(["C4/8", "E4/8", "G4/8_", "C5/4", "R/8"], tempo=240)


async def main():
    await multitask(melody(), hub.display.text("MUSIC", on=100, off=20))
    # Cancel a long tone without leaving the speaker running.
    await multitask(hub.speaker.beep(440, 1000), wait(20), race=True)


run_task(main())
print("speaker complete")
