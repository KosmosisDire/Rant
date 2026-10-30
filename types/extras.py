# What the schema cannot say in Python, pasted after the generated classes.

def identity_rotation():
    """The rotation that turns nothing, unlike the zero a default Quaternion holds."""
    return Quaternion(0.0, 0.0, 0.0, 1.0)


def identity_pose():
    return Pose(Double3(), identity_rotation())


def identity_transform():
    return Transform(identity_pose())
